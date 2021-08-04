// This file is part of Topologic software library.
// Copyright(C) 2019, Cardiff University and University College London
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "EnergyModel.h"
#include "EnergySimulation.h"

using namespace System::Diagnostics;
using namespace System::IO;
using namespace System::Linq;

namespace TopologicEnergy
{
	bool EnergyModel::CreateIdfFile(OpenStudio::Model^ osModel, String^ idfPathName)
	{
		if (idfPathName == nullptr)
		{
			throw gcnew Exception("The input idfPathName must not be null.");
		}

		OpenStudio::EnergyPlusForwardTranslator^ osForwardTranslator = gcnew OpenStudio::EnergyPlusForwardTranslator();
		OpenStudio::Workspace^ osWorkspace = osForwardTranslator->translateModel(osModel);
		OpenStudio::IdfFile^ osIdfFile = osWorkspace->toIdfFile();
		OpenStudio::Path^ osIdfPath = OpenStudio::OpenStudioUtilitiesCore::toPath(idfPathName);
		bool idfSaveCondition = osIdfFile->save(osIdfPath);
		return idfSaveCondition;
	}

	EnergyModel^ EnergyModel::ByCellComplex(
		CellComplex^ building,
		Cluster^ shadingSurfaces,
		IList<double>^ floorLevels,
		String^ buildingName,
		String^ buildingType,
		String^ defaultSpaceType,
		double northAxis,
		Nullable<double> glazingRatio,
		double coolingTemp,
		double heatingTemp,
		String^ weatherFilePath,
		String^ designDayFilePath,
		String^ openStudioTemplatePath
	)
	{
		IList<double>^ floorLevelList = (IList<double>^) floorLevels;
		numOfApertures = 0;
		numOfAppliedApertures = 0;
		CellComplex^ buildingCopy = building->Copy<CellComplex^>();

		// Create an OpenStudio model from the template, EPW, and DDY
		OpenStudio::Model^ osModel = GetModelFromTemplate(openStudioTemplatePath, weatherFilePath, designDayFilePath);

		double buildingHeight = Enumerable::Max(floorLevels);

		int numFloors = floorLevelList->Count - 1;
		OpenStudio::Building^ osBuilding = ComputeBuilding(osModel, buildingName, buildingType, buildingHeight, numFloors, northAxis, defaultSpaceType);
		IList<Cell^>^ pBuildingCells = buildingCopy->Cells;

		// Create OpenStudio spaces
		OpenStudio::SpaceVector^ osSpaceVector = gcnew OpenStudio::SpaceVector();

		Autodesk::DesignScript::Geometry::Vector^ dynamoZAxis = Autodesk::DesignScript::Geometry::Vector::ZAxis();
		for each(Cell^ buildingCell in pBuildingCells)
		{
			int spaceNumber = 1;
			OpenStudio::Space^ osSpace = AddSpace(
				spaceNumber,
				buildingCell,
				buildingCopy,
				osModel,
				dynamoZAxis,
				buildingHeight,
				floorLevels,
				glazingRatio,
				heatingTemp,
				coolingTemp
			);

			Dictionary<String^, Object^>^ attributes = gcnew Dictionary<String^, Object^>();
			attributes->Add("Name", osSpace->nameString());
			buildingCell->AddAttributesNoCopy(attributes);

			OpenStudio::SpaceVector::SpaceVectorEnumerator^ spaceEnumerator = osSpaceVector->GetEnumerator();
			while (spaceEnumerator->MoveNext())
			{
				OpenStudio::Space^ osExistingSpace = spaceEnumerator->Current;
				osSpace->matchSurfaces(osExistingSpace);
			}

			osSpaceVector->Add(osSpace);
		}
		delete dynamoZAxis;

		// Create shading surfaces
		if (shadingSurfaces != nullptr)
		{
			OpenStudio::ShadingSurfaceGroup^ osShadingGroup = gcnew OpenStudio::ShadingSurfaceGroup(osModel);
			IList<Face^>^ contextFaces = shadingSurfaces->Faces;
			int faceIndex = 1;
			for each(Face^ contextFace in contextFaces)
			{
				AddShadingSurfaces(contextFace, osModel, osShadingGroup, faceIndex++);
			}
		}

		osModel->purgeUnusedResourceObjects();

		return gcnew EnergyModel(osModel, osBuilding, pBuildingCells, shadingSurfaces, osSpaceVector);
	}

	bool EnergyModel::ExportToOSM(EnergyModel ^ energyModel, String^ filePath)
	{
		if (filePath == nullptr)
		{
			throw gcnew Exception("The input filePath must not be null.");
		}

		return SaveModel(energyModel->m_osModel, filePath);
	}

	void EnergyModel::ProcessOsModel(
		OpenStudio::Model^ osModel, 
		double tolerance, 
		OpenStudio::Building^% osBuilding,
		IList<Cell^>^% buildingCells,
		Topologic::Cluster^% shadingFaces,
		OpenStudio::SpaceVector^% osSpaceVector)
	{
		osBuilding = osModel->getBuilding(); // 2
		osSpaceVector = osModel->getSpaces(); // 3
		OpenStudio::SpaceVector::SpaceVectorEnumerator^ spaceEnumerator = osSpaceVector->GetEnumerator();
		List<OpenStudio::Space^>^ osSpaces = gcnew List<OpenStudio::Space^>();
		while (spaceEnumerator->MoveNext())
		{
			OpenStudio::Space^ osSpace = spaceEnumerator->Current;
			osSpaces->Add(osSpace);
		}

		// 4. Get shading surfaces as a cluster
		OpenStudio::ShadingSurfaceVector^ osShadingSurfaceVector = osModel->getShadingSurfaces(); //4
		OpenStudio::ShadingSurfaceVector::ShadingSurfaceVectorEnumerator^ shadingSurfaceEnumerator = osShadingSurfaceVector->GetEnumerator();
		List<Topologic::Topology^>^ shadingFaceList = gcnew List<Topologic::Topology^>();
		while (shadingSurfaceEnumerator->MoveNext())
		{
			OpenStudio::ShadingSurface^ osShadingSurface = shadingSurfaceEnumerator->Current;
			Face^ shadingFace = FaceByOsSurface(osShadingSurface);
			shadingFaceList->Add(shadingFace);
		}
		
		if (shadingFaceList->Count > 0)
		{
			shadingFaces = Topologic::Cluster::ByTopologies(shadingFaceList);
		}

		// 5. Get building spaces as a CellComplex
		OpenStudio::SpaceVector::SpaceVectorEnumerator^ osSpaceEnumerator = osSpaceVector->GetEnumerator();
		List<Topologic::Cell^>^ cellList = gcnew List<Topologic::Cell^>();
		while (osSpaceEnumerator->MoveNext())
		{
			OpenStudio::Space^ osSpace = osSpaceEnumerator->Current;
			OpenStudio::SurfaceVector^ osSurfaces = osSpace->surfaces;
			OpenStudio::Transformation^ osTransformation = osSpace->transformation();
			OpenStudio::Vector3d^ osTranslation = osTransformation->translation();
			OpenStudio::Matrix^ osMatrix = osTransformation->rotationMatrix();
			double rotation11 = osMatrix->__getitem__(0, 0);
			double rotation12 = osMatrix->__getitem__(0, 1);
			double rotation13 = osMatrix->__getitem__(0, 2);
			double rotation21 = osMatrix->__getitem__(1, 0);
			double rotation22 = osMatrix->__getitem__(1, 1);
			double rotation23 = osMatrix->__getitem__(1, 2);
			double rotation31 = osMatrix->__getitem__(2, 0);
			double rotation32 = osMatrix->__getitem__(2, 1);
			double rotation33 = osMatrix->__getitem__(2, 2);

			OpenStudio::SurfaceVector::SurfaceVectorEnumerator^ osSurfaceEnumerator = osSurfaces->GetEnumerator();
			List<Topologic::Face^>^ faceList = gcnew List<Topologic::Face^>();
			while (osSurfaceEnumerator->MoveNext())
			{
				OpenStudio::Surface^ osSurface = osSurfaceEnumerator->Current;
				Face^ face = FaceByOsSurface(osSurface);

				// Subsurfaces
				OpenStudio::SubSurfaceVector^ osSubSurfaces = osSurface->subSurfaces();
				if (osSubSurfaces->Count > 0)
				{
					OpenStudio::SubSurfaceVector::SubSurfaceVectorEnumerator^ osSubSurfaceEnumerator = osSubSurfaces->GetEnumerator();
					List<Topologic::Topology^>^ faceApertureList = gcnew List<Topologic::Topology^>();
					while (osSubSurfaceEnumerator->MoveNext())
					{
						OpenStudio::SubSurface^ osSubSurface = osSubSurfaceEnumerator->Current;
						Face^ subFace = FaceByOsSurface(osSubSurface);

						faceApertureList->Add(subFace);
					}
					Topologic::Topology^ topologyWithApertures = face->AddApertures(faceApertureList);
					try {
						Face^ faceWithApertures = safe_cast<Topologic::Face^>(topologyWithApertures);

						faceList->Add(faceWithApertures);
					}
					catch (Exception^)
					{
						throw gcnew Exception("Error converting a topology with apertures to a face.");
					}
				}
				else // no subsurfaces
				{
					faceList->Add(face);
				}
			}

			Cell^ cell = Cell::ByFaces(faceList, tolerance);

			Topologic::Topology^ transformedTopology = Topologic::Utilities::TopologyUtility::Transform(cell,
				osTranslation->x(), osTranslation->y(), osTranslation->z(),
				rotation11, rotation12, rotation13,
				rotation21, rotation22, rotation23,
				rotation31, rotation32, rotation33);
			Cell^ transformedCell = safe_cast<Topologic::Cell^>(transformedTopology);
			cellList->Add(transformedCell);
		}

		buildingCells = gcnew List<Topologic::Cell^>();
		if (cellList->Count == 1)
		{
			((List<Topologic::Cell^>^)buildingCells)->Add(cellList[0]);
		}
		else
		{
			CellComplex^ cellComplex = CellComplex::ByCells(cellList);
			buildingCells = cellComplex->Cells;
		}
	}

	EnergyModel ^ EnergyModel::ByImportedOSM(String ^ filePath, double tolerance)
	{
		if (filePath == nullptr)
		{
			throw gcnew Exception("The input filePath must not be null.");
		}

		if (tolerance <= 0.0)
		{
			throw gcnew Exception("The tolerance must have a positive value.");
		}

		OpenStudio::Path^ osOsmFile = OpenStudio::OpenStudioUtilitiesCore::toPath(filePath);

		// Create an abstract model
		OpenStudio::OptionalModel^ osOptionalModel = OpenStudio::Model::load(osOsmFile);
		if (osOptionalModel->isNull())
		{
			return nullptr;
		}

		OpenStudio::Model^ osModel = osOptionalModel->get(); //1
		if (osModel == nullptr)
		{
			return nullptr;
		}

		OpenStudio::Building^ osBuilding = nullptr;
		List<Cell^>^ buildingCells = nullptr;
		Cluster^ shadingFaces = nullptr;
		OpenStudio::SpaceVector^ osSpaceVector = nullptr;
		ProcessOsModel(osModel, tolerance, osBuilding, buildingCells, shadingFaces, osSpaceVector);

		EnergyModel^ energyModel = gcnew EnergyModel(osModel, osBuilding, buildingCells, shadingFaces, osSpaceVector);

		return energyModel;
	}

	bool EnergyModel::SaveModel(OpenStudio::Model^ osModel, String^ osmPathName)
	{
		if (osmPathName == nullptr)
		{
			throw gcnew Exception("The input osmPathName must not be null.");
		}

		// Purge unused resources
		//osModel->purgeUnusedResourceObjects();

		// Create a path string
		OpenStudio::Path^ osPath = OpenStudio::OpenStudioUtilitiesCore::toPath(osmPathName);
		bool osCondition = osModel->save(osPath, true);
		return osCondition;
	}

	OpenStudio::Model^ EnergyModel::GetModelFromTemplate(String^ osmTemplatePath, String^ epwWeatherPath, String^ ddyPath)
	{
		if (osmTemplatePath == nullptr)
		{
			throw gcnew Exception("The input osmTemplatePath must not be null.");
		}

		if (epwWeatherPath == nullptr)
		{
			throw gcnew Exception("The input epwWeatherPath must not be null.");
		}

		if (ddyPath == nullptr)
		{
			throw gcnew Exception("The input ddyPath must not be null.");
		}

		if (osmTemplatePath == nullptr)
		{
			throw gcnew Exception("The input osmTemplatePath must not be null.");
		}

		if (!File::Exists(osmTemplatePath))
		{
			throw gcnew FileNotFoundException("OSM file not found.");
		}
		if (!File::Exists(epwWeatherPath))
		{
			throw gcnew FileNotFoundException("EPW file not found.");
		}
		if (!File::Exists(ddyPath))
		{
			throw gcnew FileNotFoundException("DDY file not found.");
		}
		OpenStudio::Path^ osTemplatePath = OpenStudio::OpenStudioUtilitiesCore::toPath(osmTemplatePath);

		// Create an abstract model
		OpenStudio::OptionalModel^ osOptionalModel = OpenStudio::Model::load(osTemplatePath);
		OpenStudio::Model^ osModel = osOptionalModel->__ref__();

		// Read an EPW weather file
		OpenStudio::Path^ osEPWPath = OpenStudio::OpenStudioUtilitiesCore::toPath(epwWeatherPath);
		OpenStudio::EpwFile^ osEPWFile = gcnew OpenStudio::EpwFile(osEPWPath);
		OpenStudio::WeatherFile^ osWeatherFile = osModel->getWeatherFile();
		OpenStudio::WeatherFile::setWeatherFile(osModel, osEPWFile);

		// Read an DDY design days files
		OpenStudio::Path^ osDDYPath = OpenStudio::OpenStudioUtilitiesCore::toPath(ddyPath);
		OpenStudio::EnergyPlusReverseTranslator^ osTranslator = gcnew OpenStudio::EnergyPlusReverseTranslator();
		OpenStudio::OptionalModel^ tempModel01 = osTranslator->loadModel(osDDYPath);
		OpenStudio::Model^ tempModel02 = tempModel01->__ref__();
		OpenStudio::DesignDayVector^ designDays = tempModel02->getDesignDays();
		OpenStudio::DesignDayVector::DesignDayVectorEnumerator^ designDaysEnumerator = designDays->GetEnumerator();

		while (designDaysEnumerator->MoveNext())
		{
			OpenStudio::DesignDay^ aDesignDay = designDaysEnumerator->Current;
			OpenStudio::IdfObject^ anIdfObject = aDesignDay->idfObject();
			osModel->addObject(anIdfObject);
		}
		return osModel;
	}

	OpenStudio::ThermalZone^ EnergyModel::CreateThermalZone(OpenStudio::Model^ model, OpenStudio::Space^ space, double ceilingHeight, double heatingTemp, double coolingTemp)
	{
		// Create a thermal zone for the space
		OpenStudio::ThermalZone^ osThermalZone = gcnew OpenStudio::ThermalZone(model);
		osThermalZone->setName(space->name()->get() + "_THERMAL_ZONE");
		String^ name = osThermalZone->name()->get();
		osThermalZone->setUseIdealAirLoads(true);
		osThermalZone->setCeilingHeight(ceilingHeight);
		osThermalZone->setVolume(space->volume());

		// Assign Thermal Zone to space
		// aSpace.setThermalZone(osThermalZone);//Not available in C#
		OpenStudio::UUID^ tzHandle = osThermalZone->handle();
		int location = 10;
		space->setPointer(location, tzHandle);

		OpenStudio::ScheduleConstant^ heatingScheduleConstant = gcnew OpenStudio::ScheduleConstant(model);
		heatingScheduleConstant->setValue(heatingTemp);
		OpenStudio::ScheduleConstant^ coolingScheduleConstant = gcnew OpenStudio::ScheduleConstant(model);
		coolingScheduleConstant->setValue(coolingTemp);

		// Create a Thermostat
		OpenStudio::ThermostatSetpointDualSetpoint^ osThermostat = gcnew OpenStudio::ThermostatSetpointDualSetpoint(model);

		// Set Heating and Cooling Schedules on the Thermostat
		osThermostat->setHeatingSetpointTemperatureSchedule(heatingScheduleConstant);
		osThermostat->setCoolingSetpointTemperatureSchedule(coolingScheduleConstant);

		// Assign Thermostat to the Thermal Zone
		osThermalZone->setThermostatSetpointDualSetpoint(osThermostat);
		return osThermalZone;
	}

	OpenStudio::BuildingStory^ EnergyModel::AddBuildingStory(OpenStudio::Model^ model, int floorNumber)
	{
		OpenStudio::BuildingStory^ osBuildingStory = gcnew OpenStudio::BuildingStory(model);
		osBuildingStory->setName("STORY_" + floorNumber);
		osBuildingStory->setDefaultConstructionSet(getDefaultConstructionSet(model));
		osBuildingStory->setDefaultScheduleSet(getDefaultScheduleSet(model));
		return osBuildingStory;
	}

	OpenStudio::SubSurface ^ EnergyModel::CreateSubSurface(IList<Topologic::Vertex^>^ vertices, OpenStudio::Model^ osModel)
	{
		OpenStudio::Point3dVector^ osWindowFacePoints = gcnew OpenStudio::Point3dVector();
		for each(Vertex^ vertex in vertices)
		{
			OpenStudio::Point3d^ osPoint = gcnew OpenStudio::Point3d(
				vertex->X,
				vertex->Y,
				vertex->Z);
			osWindowFacePoints->Add(osPoint);
		}

		OpenStudio::SubSurface^ osWindowSubSurface = gcnew OpenStudio::SubSurface(osWindowFacePoints, osModel);
		return osWindowSubSurface;
	}

	OpenStudio::Building^ EnergyModel::ComputeBuilding(
		OpenStudio::Model^ osModel,
		String^ buildingName,
		String^ buildingType,
		double buildingHeight,
		int numFloors,
		double northAxis,
		String^ spaceType)
	{
		OpenStudio::Building^ osBuilding = osModel->getBuilding();
		osBuilding->setStandardsNumberOfStories(numFloors);
		osBuilding->setDefaultConstructionSet(getDefaultConstructionSet(osModel));
		osBuilding->setDefaultScheduleSet(getDefaultScheduleSet(osModel));
		osBuilding->setName(buildingName);
		osBuilding->setStandardsBuildingType(buildingType);
		double floorToFloorHeight = (double)buildingHeight / (double)numFloors;
		osBuilding->setNominalFloortoFloorHeight(floorToFloorHeight);
		// Get all space types and find the one that matches
		OpenStudio::SpaceTypeVector^ spaceTypes = osModel->getSpaceTypes();
		OpenStudio::SpaceTypeVector::SpaceTypeVectorEnumerator^ spaceTypesEnumerator = spaceTypes->GetEnumerator();
		while (spaceTypesEnumerator->MoveNext())
		{
			OpenStudio::SpaceType^ aSpaceType = spaceTypesEnumerator->Current;
			String^ spaceTypeName = aSpaceType->name()->__str__();
			if (spaceTypeName == spaceType)
			{
				osBuilding->setSpaceType(aSpaceType);
			}
		}
		buildingStories = CreateBuildingStories(osModel, numFloors);
		osBuilding->setNorthAxis(northAxis);
		return osBuilding;
	}

	IList<OpenStudio::BuildingStory^>^ EnergyModel::CreateBuildingStories(OpenStudio::Model^ osModel, int numFloors)
	{
		List<OpenStudio::BuildingStory^>^ osBuildingStories = gcnew List<OpenStudio::BuildingStory^>();
		for (int i = 0; i < numFloors; i++)
		{
			osBuildingStories->Add(AddBuildingStory(osModel, (i + 1)));
		}
		return osBuildingStories;
	}

	OpenStudio::SqlFile^ EnergyModel::CreateSqlFile(OpenStudio::Model ^ osModel, String^ sqlFilePath)
	{
		if (sqlFilePath == nullptr)
		{
			throw gcnew Exception("The input sqlFilePath must not be null.");
		}

		OpenStudio::Path^ osSqlFilePath = OpenStudio::OpenStudioUtilitiesCore::toPath(sqlFilePath);
		OpenStudio::SqlFile^ osSqlFile = gcnew OpenStudio::SqlFile(osSqlFilePath);
		if (osSqlFile == nullptr)
		{
			throw gcnew Exception("Fails to create an SQL output file");
		}
		bool isSuccessful = osModel->setSqlFile(osSqlFile);
		if (!isSuccessful)
		{
			throw gcnew Exception("Fails to create an SQL output file");
		}
		return osSqlFile;
	}

	Topologic::Face ^ EnergyModel::FaceByOsSurface(OpenStudio::PlanarSurface^ osPlanarSurface)
	{
		OpenStudio::Point3dVector^ osVertices = osPlanarSurface->vertices();
		OpenStudio::Point3dVector::Point3dVectorEnumerator^ osPointEnumerator = osVertices->GetEnumerator();
		List<Topologic::Vertex^>^ vertices = gcnew List<Topologic::Vertex^>();
		List<int>^ indices = gcnew List<int>();
		int index = 0;
		while (osPointEnumerator->MoveNext())
		{
			OpenStudio::Point3d^ osVertex = osPointEnumerator->Current;
			vertices->Add(Topologic::Vertex::ByCoordinates(osVertex->x(), osVertex->y(), osVertex->z()));
			indices->Add(index);
			++index;
		}

		if (vertices->Count < 3)
		{
			throw gcnew Exception("Invalid surface is found.");
		}

		indices->Add(0); // close the wire

		IList<IList<int>^>^ vertexIndices = gcnew List<IList<int>^>();
		((IList<IList<int>^>^)vertexIndices)->Add(indices);

		IList<Topologic::Topology^>^ topologies = (IList<Topologic::Topology^>^)Topologic::Topology::ByVerticesIndices(vertices, vertexIndices);

		if (topologies == nullptr || topologies->Count == 0)
		{
			throw gcnew Exception("Error creating a topology from a surface.");
		}

		Topologic::Face^ face = nullptr;
		try {
			face = safe_cast<Topologic::Face^>(topologies[0]);
		}
		catch (Exception^)
		{
			throw gcnew Exception("Error converting a topology to a face.");
		}

		return face;
	}

	double EnergyModel::DoubleValueFromQuery(OpenStudio::SqlFile^ sqlFile, String^ EPReportName, String^ EPReportForString, String^ EPTableName, String^ EPColumnName, String^ EPRowName, String^ EPUnits)
	{
		double doubleValue = 0.0;
		String^ query = "SELECT Value FROM tabulardatawithstrings WHERE ReportName='" + EPReportName + "' AND ReportForString='" + EPReportForString + "' AND TableName = '" + EPTableName + "' AND RowName = '" + EPRowName + "' AND ColumnName= '" + EPColumnName + "' AND Units='" + EPUnits + "'";
		OpenStudio::OptionalDouble^ osDoubleValue = sqlFile->execAndReturnFirstDouble(query);
		if (osDoubleValue->is_initialized())
		{
			doubleValue = osDoubleValue->get();
		}
		else
		{
			throw gcnew Exception("Fails to get a double value from the SQL file.");
		}
		return doubleValue;
	}

	String^ EnergyModel::StringValueFromQuery(OpenStudio::SqlFile^ sqlFile, String^ EPReportName, String^ EPReportForString, String^ EPTableName, String^ EPColumnName, String^ EPRowName, String^ EPUnits)
	{
		String^ query = "SELECT Value FROM tabulardatawithstrings WHERE ReportName='" + EPReportName + "' AND ReportForString='" + EPReportForString + "' AND TableName = '" + EPTableName + "' AND RowName = '" + EPRowName + "' AND ColumnName= '" + EPColumnName + "' AND Units='" + EPUnits + "'";
		return sqlFile->execAndReturnFirstString(query)->get();
	}

	int EnergyModel::IntValueFromQuery(OpenStudio::SqlFile^ sqlFile, String^ EPReportName, String^ EPReportForString, String^ EPTableName, String^ EPColumnName, String^ EPRowName, String^ EPUnits)
	{
		String^ query = "SELECT Value FROM tabulardatawithstrings WHERE ReportName='" + EPReportName + "' AND ReportForString='" + EPReportForString + "' AND TableName = '" + EPTableName + "' AND RowName = '" + EPRowName + "' AND ColumnName= '" + EPColumnName + "' AND Units='" + EPUnits + "'";
		return sqlFile->execAndReturnFirstInt(query)->get();
	}

	bool EnergyModel::Export(EnergyModel ^ energyModel, String ^ openStudioOutputDirectory, String ^% oswPath)
	{
		// Add timestamp to the output file name
		String^ openStudioOutputTimeStampPath = Path::GetDirectoryName(openStudioOutputDirectory + "\\") + "\\" +
			Path::GetFileNameWithoutExtension(energyModel->BuildingName) +
			".osm";
		// Save model to an OSM file
		bool saveCondition = SaveModel(energyModel->m_osModel, openStudioOutputTimeStampPath);

		if (!saveCondition)
		{
			return false;
		}

		OpenStudio::WorkflowJSON^ workflow = gcnew OpenStudio::WorkflowJSON();
		try {
			String^ osmFilename = Path::GetFileNameWithoutExtension(openStudioOutputTimeStampPath);
			String^ osmDirectory = Path::GetDirectoryName(openStudioOutputTimeStampPath);
			oswPath = osmDirectory + "\\" + osmFilename + ".osw";
			OpenStudio::Path^ osOswPath = OpenStudio::OpenStudioUtilitiesCore::toPath(oswPath);
			workflow->setSeedFile(OpenStudio::OpenStudioUtilitiesCore::toPath(openStudioOutputTimeStampPath));
			workflow->setWeatherFile(gcnew OpenStudio::Path());
			workflow->saveAs(osOswPath);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}


	OpenStudio::DefaultScheduleSet^ EnergyModel::getDefaultScheduleSet(OpenStudio::Model^ model)
	{
		// Get list of default schedule sets
		OpenStudio::DefaultScheduleSetVector^ defaultScheduleSets = model->getDefaultScheduleSets();
		OpenStudio::DefaultScheduleSetVector::DefaultScheduleSetVectorEnumerator^ defSchedEnum = defaultScheduleSets->GetEnumerator();
		defSchedEnum->MoveNext();
		defaultScheduleSet = defSchedEnum->Current;
		return defaultScheduleSet;
	}

	OpenStudio::DefaultConstructionSet^ EnergyModel::getDefaultConstructionSet(OpenStudio::Model ^ model)
	{
		// Get list of default construction sets
		OpenStudio::DefaultConstructionSetVector^ defaultConstructionSets = model->getDefaultConstructionSets();
		// Get the first item and use as the default construction set
		OpenStudio::DefaultConstructionSetVector::DefaultConstructionSetVectorEnumerator^ defConEnum = defaultConstructionSets->GetEnumerator();
		defConEnum->MoveNext();
		defaultConstructionSet = defConEnum->Current;
		return defaultConstructionSet;
	}

	OpenStudio::Space^ EnergyModel::AddSpace(
		int spaceNumber,
		Cell^ cell,
		CellComplex^ cellComplex,
		OpenStudio::Model^ osModel,
		Autodesk::DesignScript::Geometry::Vector^ upVector,
		double buildingHeight,
		IList<double>^ floorLevels,
		Nullable<double> glazingRatio,
		double heatingTemp,
		double coolingTemp)
	{
		OpenStudio::Space^ osSpace = gcnew OpenStudio::Space(osModel);

		int storyNumber = StoryNumber(cell, buildingHeight, floorLevels);
		OpenStudio::BuildingStory^ buildingStory = ((IList< OpenStudio::BuildingStory^>^)buildingStories)[storyNumber];
		osSpace->setName(buildingStory->name()->get() + "_SPACE_" + spaceNumber.ToString());
		osSpace->setBuildingStory(buildingStory);
		osSpace->setDefaultConstructionSet(getDefaultConstructionSet(osModel));
		osSpace->setDefaultScheduleSet(getDefaultScheduleSet(osModel));

		IList<Face^>^ faces = (IList<Face^>^)cell->Faces;
		List<OpenStudio::Point3dVector^>^ facePointsList = gcnew List<OpenStudio::Point3dVector^>();
		for each(Face^ face in faces)
		{
			OpenStudio::Point3dVector^ facePoints = GetFacePoints(face);
			facePointsList->Add(facePoints);
		}

		for (int i = 0; i < faces->Count; ++i)
		{
			AddSurface(i + 1, faces[i], cell, cellComplex, facePointsList[i], osSpace, osModel, upVector, glazingRatio);
		}

		// Get all space types
		OpenStudio::SpaceTypeVector^ osSpaceTypes = osModel->getSpaceTypes();
		OpenStudio::SpaceTypeVector::SpaceTypeVectorEnumerator^ osSpaceTypesEnumerator = osSpaceTypes->GetEnumerator();
		int spaceTypeCount = osSpaceTypes->Count;
		while (osSpaceTypesEnumerator->MoveNext())
		{
			OpenStudio::SpaceType^ osSpaceType = osSpaceTypesEnumerator->Current;
			OpenStudio::OptionalString^ osSpaceTypeOptionalString = osSpaceType->name();
			String^ spaceTypeName = osSpaceTypeOptionalString->__str__();
			if (spaceTypeName == "ASHRAE 189::1-2009 ClimateZone 4-8 MediumOffice")
			{
				osSpace->setSpaceType(osSpaceType);
			}
		}

		IList<double>^ minMax = (IList<double>^)Topologic::Utilities::CellUtility::GetMinMax(cell);
		double minZ = minMax[4];
		double maxZ = minMax[5];
		double ceilingHeight = Math::Abs(maxZ - minZ);

		OpenStudio::ThermalZone^ thermalZone = CreateThermalZone(osModel, osSpace, ceilingHeight, heatingTemp, coolingTemp);

		return osSpace;
	}


	void EnergyModel::AddShadingSurfaces(Cell^ buildingCell, OpenStudio::Model^ osModel)
	{
		OpenStudio::ShadingSurfaceGroup^ osShadingGroup = gcnew OpenStudio::ShadingSurfaceGroup(osModel);
		IList<Face^>^ faceList = buildingCell->Faces;
		int faceIndex = 1;
		for each(Face^ face in faceList)
		{
			IList<Vertex^>^ vertices = face->Vertices;
			OpenStudio::Point3dVector^ facePoints = gcnew OpenStudio::Point3dVector();

			for each(Vertex^ aVertex in vertices)
			{
				OpenStudio::Point3d^ aPoint = gcnew OpenStudio::Point3d(aVertex->X, aVertex->Y, aVertex->Z);
				facePoints->Add(aPoint);
			}

			OpenStudio::ShadingSurface^ aShadingSurface = gcnew OpenStudio::ShadingSurface(facePoints, osModel);

			String^ surfaceName = buildingCell->ToString() + "_SHADINGSURFACE_" + (faceIndex.ToString());
			aShadingSurface->setName(surfaceName);
			aShadingSurface->setShadingSurfaceGroup(osShadingGroup);

			++faceIndex;
		}
	}

	void EnergyModel::AddShadingSurfaces(Face ^ buildingFace, OpenStudio::Model ^ osModel, OpenStudio::ShadingSurfaceGroup^ osShadingGroup, int faceIndex)
	{
		IList<Vertex^>^ vertices = buildingFace->Vertices;
		OpenStudio::Point3dVector^ facePoints = gcnew OpenStudio::Point3dVector();

		for each(Vertex^ aVertex in vertices)
		{
			OpenStudio::Point3d^ aPoint = gcnew OpenStudio::Point3d(aVertex->X, aVertex->Y, aVertex->Z);
			facePoints->Add(aPoint);
		}

		OpenStudio::ShadingSurface^ aShadingSurface = gcnew OpenStudio::ShadingSurface(facePoints, osModel);

		String^ surfaceName = "SHADINGSURFACE_" + (faceIndex.ToString());
		aShadingSurface->setName(surfaceName);
		aShadingSurface->setShadingSurfaceGroup(osShadingGroup);
	}

	OpenStudio::Surface^ EnergyModel::AddSurface(
		int surfaceNumber,
		Face^ buildingFace,
		Cell^ buildingSpace,
		CellComplex^ cellComplex,
		OpenStudio::Point3dVector^ osFacePoints,
		OpenStudio::Space^ osSpace,
		OpenStudio::Model^ osModel,
		Autodesk::DesignScript::Geometry::Vector^ upVector,
		[Autodesk::DesignScript::Runtime::DefaultArgument("null")] Nullable<double> glazingRatio)
	{
		OpenStudio::Construction^ osInteriorCeilingType = nullptr;
		OpenStudio::Construction^ osExteriorRoofType = nullptr;
		OpenStudio::Construction^ osInteriorFloorType = nullptr;
		OpenStudio::Construction^ osInteriorWallType = nullptr;
		OpenStudio::Construction^ osExteriorDoorType = nullptr;
		OpenStudio::Construction^ osExteriorWallType = nullptr;
		OpenStudio::Construction^ osExteriorWindowType = nullptr;
		int subsurfaceCounter = 1;

		OpenStudio::ConstructionVector^ osConstructionTypes = osModel->getConstructions();
		OpenStudio::ConstructionVector::ConstructionVectorEnumerator^ osConstructionTypesEnumerator =
			osConstructionTypes->GetEnumerator();
		int constructionTypeCount = osConstructionTypes->Count;

		while (osConstructionTypesEnumerator->MoveNext())
		{
			OpenStudio::Construction^ osConstruction = osConstructionTypesEnumerator->Current;
			OpenStudio::OptionalString^ osConstructionTypeOptionalString = osConstruction->name();
			String^ constructionTypeName = osConstructionTypeOptionalString->__str__();
			if (constructionTypeName->Equals("000 Interior Ceiling"))
			{
				osInteriorCeilingType = osConstruction;
			}
			else if (constructionTypeName->Equals("000 Interior Floor"))
			{
				osInteriorFloorType = osConstruction;
			}
			else if (constructionTypeName->Equals("000 Interior Wall"))
			{
				osInteriorWallType = osConstruction;
			}
			else if (constructionTypeName->Equals("ASHRAE 189.1-2009 ExtWindow ClimateZone 4-5"))
			{
				osExteriorWindowType = osConstruction;
			}
			else if (constructionTypeName->Equals("000 Exterior Door"))
			{
				osExteriorDoorType = osConstruction;
			}
			else if (constructionTypeName->Equals("ASHRAE 189.1-2009 ExtRoof IEAD ClimateZone 2-5"))
			{
				osExteriorRoofType = osConstruction;
			}
			else if (constructionTypeName->Equals("ASHRAE 189.1-2009 ExtWall SteelFrame ClimateZone 4-8"))
			{
				osExteriorWallType = osConstruction;
			}
		} // while (osConstructionTypesEnumerator.MoveNext())

		int adjCount = AdjacentCellCount(buildingFace);
		//HACK
		/*if (adjCount > 1)
		{
			osFacePoints->Reverse();
		}*/

		OpenStudio::Surface^ osSurface = gcnew OpenStudio::Surface(osFacePoints, osModel);
		osSurface->setSpace(osSpace);
		OpenStudio::OptionalString^ osSpaceOptionalString = osSpace->name();
		String^ spaceName = osSpace->name()->get();
		String^ surfaceName = osSpace->name()->get() + "_SURFACE_" + surfaceNumber.ToString();
		bool isUnderground = IsUnderground(buildingFace);
		FaceType faceType = CalculateFaceType(buildingFace, osFacePoints, buildingSpace, upVector);
		osSurface->setName(surfaceName);

		if ((faceType == FACE_ROOFCEILING) && (adjCount > 1))
		{

			osSurface->setOutsideBoundaryCondition("Surface");
			osSurface->setSurfaceType("RoofCeiling");
			osSurface->setConstruction(osInteriorCeilingType);
			osSurface->setSunExposure("NoSun");
			osSurface->setWindExposure("NoWind");
		}
		else if ((faceType == FACE_ROOFCEILING) && (adjCount < 2) && (!isUnderground))
		{
			OpenStudio::Vector3d^ pSurfaceNormal = osSurface->outwardNormal();

			double x = pSurfaceNormal->x();
			double y = pSurfaceNormal->y();
			double z = pSurfaceNormal->z();
			pSurfaceNormal->normalize();
			OpenStudio::Vector3d^ upVector = gcnew OpenStudio::Vector3d(0, 0, 1.0);
			// If the normal does not point downward, flip it. Use dot product.
			double dotProduct = pSurfaceNormal->dot(upVector);
			if (dotProduct < 0.98)
			{
				OpenStudio::Point3dVector^ surfaceVertices = osSurface->vertices();
				surfaceVertices->Reverse();
				osSurface->setVertices(surfaceVertices);
			}
			osSurface->setOutsideBoundaryCondition("Outdoors");
			osSurface->setSurfaceType("RoofCeiling");
			osSurface->setConstruction(osExteriorRoofType);
			osSurface->setSunExposure("SunExposed");
			osSurface->setWindExposure("WindExposed");
		}
		else if ((faceType == FACE_ROOFCEILING) && (adjCount < 2) && isUnderground)
		{
			OpenStudio::Vector3d^ pSurfaceNormal = osSurface->outwardNormal();

			double x = pSurfaceNormal->x();
			double y = pSurfaceNormal->y();
			double z = pSurfaceNormal->z();
			pSurfaceNormal->normalize();
			OpenStudio::Vector3d^ upVector = gcnew OpenStudio::Vector3d(0, 0, 1.0);
			// If the normal does not point downward, flip it. Use dot product.
			double dotProduct = pSurfaceNormal->dot(upVector);
			if (dotProduct < 0.98)
			{
				OpenStudio::Point3dVector^ surfaceVertices = osSurface->vertices();
				surfaceVertices->Reverse();
				osSurface->setVertices(surfaceVertices);
			}
			osSurface->setOutsideBoundaryCondition("Ground");
			osSurface->setSurfaceType("RoofCeiling");
			osSurface->setConstruction(osExteriorRoofType);
			osSurface->setSunExposure("NoSun");
			osSurface->setWindExposure("NoWind");
		}
		else if ((faceType == FACE_FLOOR) && (adjCount > 1))
		{
			osSurface->setOutsideBoundaryCondition("Surface");
			osSurface->setSurfaceType("Floor");
			osSurface->setConstruction(osInteriorFloorType);
			osSurface->setSunExposure("NoSun");
			osSurface->setWindExposure("NoWind");
		}
		else if ((faceType == FACE_FLOOR) && (adjCount < 2))
		{
			OpenStudio::Vector3d^ pSurfaceNormal = osSurface->outwardNormal();

			double x = pSurfaceNormal->x();
			double y = pSurfaceNormal->y();
			double z = pSurfaceNormal->z();
			pSurfaceNormal->normalize();
			OpenStudio::Vector3d^ downwardVector = gcnew OpenStudio::Vector3d(0, 0, -1.0);
			// If the normal does not point downward, flip it. Use dot product.
			double dotProduct = pSurfaceNormal->dot(downwardVector);
			if (dotProduct < 0.98)
			{
				OpenStudio::Point3dVector^ surfaceVertices = osSurface->vertices();
				surfaceVertices->Reverse();
				osSurface->setVertices(surfaceVertices);
			}
			osSurface->setOutsideBoundaryCondition("Ground");
			osSurface->setSurfaceType("Floor");
			osSurface->setConstruction(osExteriorWallType);
			osSurface->setSunExposure("NoSun");
			osSurface->setWindExposure("NoWind");
		}
		else if ((faceType == FACE_WALL) && (adjCount > 1)) // internal wall
		{
			osSurface->setOutsideBoundaryCondition("Surface");
			osSurface->setSurfaceType("Wall");
			osSurface->setConstruction(osInteriorWallType);
			osSurface->setSunExposure("NoSun");
			osSurface->setWindExposure("NoWind");
		}
		else if ((faceType == FACE_WALL) && (adjCount < 2) && isUnderground) // external wall underground
		{
			osSurface->setOutsideBoundaryCondition("Ground");
			osSurface->setSurfaceType("Wall");
			osSurface->setConstruction(osExteriorWallType);
			osSurface->setSunExposure("NoSun");
			osSurface->setWindExposure("NoWind");
		}
		else if ((faceType == FACE_WALL) && (adjCount < 2) && (!isUnderground)) // external wall overground
		{
			osSurface->setOutsideBoundaryCondition("Outdoors");
			osSurface->setSurfaceType("Wall");
			osSurface->setConstruction(osExteriorWallType);
			osSurface->setSunExposure("SunExposed");
			osSurface->setWindExposure("WindExposed");

			if (glazingRatio.HasValue)
			{
				if (glazingRatio.Value < 0.0 || glazingRatio.Value > 1.0)
				{
					throw gcnew Exception("The glazing ratio must be between 0.0 and 1.0 (both inclusive).");
				}
				else if (glazingRatio.Value > 0.0 && glazingRatio.Value <= 1.0)
				{
					// Triangulate the Windows
					IList<Vertex^>^ scaledVertices = (IList<Vertex^>^)ScaleFaceVertices(buildingFace, glazingRatio.Value);

					for (int i = 0; i < scaledVertices->Count - 2; ++i)
					{
						List<Vertex^>^ triangleVertices = gcnew List<Vertex^>();
						triangleVertices->Add(scaledVertices[0]);
						triangleVertices->Add(scaledVertices[i + 1]);
						triangleVertices->Add(scaledVertices[i + 2]);

						IList<Vertex^>^ scaledTriangleVertices = ScaleVertices(triangleVertices, 0.999);

						OpenStudio::Point3dVector^ osWindowFacePoints = gcnew OpenStudio::Point3dVector();

						for each (Vertex^ scaledTriangleVertex in scaledTriangleVertices)
						{
							OpenStudio::Point3d^ osPoint = gcnew OpenStudio::Point3d(
								scaledTriangleVertex->X,
								scaledTriangleVertex->Y,
								scaledTriangleVertex->Z);

							osWindowFacePoints->Add(osPoint);
						}

						OpenStudio::SubSurface^ osWindowSubSurface = gcnew OpenStudio::SubSurface(osWindowFacePoints, osModel);
						double dotProduct = osWindowSubSurface->outwardNormal()->dot(osSurface->outwardNormal());
						if (dotProduct < -0.99) // flipped
						{
							osWindowFacePoints->Reverse();
							osWindowSubSurface->remove();
							osWindowSubSurface = gcnew OpenStudio::SubSurface(osWindowFacePoints, osModel); //CreateSubSurface(pApertureVertices, osModel);
						}
						else if (dotProduct > -0.99 && dotProduct < 0.99)
						{
							throw gcnew Exception("There is a non-coplanar subsurface.");
						}
						osWindowSubSurface->setSubSurfaceType("FixedWindow");
						osWindowSubSurface->setSurface(osSurface);
						osWindowSubSurface->setName(osSurface->name()->get() + "_SUBSURFACE_" + subsurfaceCounter.ToString());
						subsurfaceCounter++;
					} // for (int i = 0; i < scaledVertices->Count - 2; ++i)
				}
			}
			else // glazingRatio is null
			{
				// Use the surface apertures
				IList<Topologic::Topology^>^ pContents = buildingFace->Contents;
				for each(Topologic::Topology^ pContent in pContents)
				{
					Aperture^ pAperture = dynamic_cast<Aperture^>(pContent);
					if (pAperture == nullptr)
					{
						continue;
					}

					Face^ pFaceAperture = dynamic_cast<Face^>(pAperture->Topology);
					if (pAperture == nullptr)
					{
						continue;
					}
					// skip small triangles
					double area = Topologic::Utilities::FaceUtility::Area(pFaceAperture);
					if (area <= 0.1)
					{
						continue;
					}
					Wire^ pApertureWire = pFaceAperture->ExternalBoundary;
					List<Vertex^>^ pApertureVertices = (List<Vertex^>^)pApertureWire->Vertices;
					//OpenStudio::SubSurface^ osWindowSubSurface = gcnew OpenStudio::SubSurface(osWindowFacePoints, osModel);
					OpenStudio::SubSurface^ osWindowSubSurface = CreateSubSurface(pApertureVertices, osModel);
					double dotProduct = osWindowSubSurface->outwardNormal()->dot(osSurface->outwardNormal());
					if (dotProduct < -0.99) // flipped
					{
						pApertureVertices->Reverse();
						osWindowSubSurface->remove();
						osWindowSubSurface = CreateSubSurface(pApertureVertices, osModel);
					}
					else if (dotProduct > -0.99 && dotProduct < 0.99)
					{
						throw gcnew Exception("There is a non-coplanar subsurface.");
					}

					numOfApertures++;

					double grossSubsurfaceArea = osWindowSubSurface->grossArea();
					double netSubsurfaceArea = osWindowSubSurface->netArea();
					double grossSurfaceArea = osSurface->grossArea();
					double netSurfaceArea = osSurface->netArea();
					if (grossSubsurfaceArea > 0.1)
					{
						osWindowSubSurface->setSubSurfaceType("FixedWindow");
						bool result = osWindowSubSurface->setSurface(osSurface);
						if (result)
						{
							osWindowSubSurface->setName(osSurface->name()->get() + "_SUBSURFACE_" + subsurfaceCounter.ToString());
							subsurfaceCounter++;
							numOfAppliedApertures++;
						}
					}
					else
					{
						osWindowSubSurface->remove();
					}
				}
			}
		}

		return osSurface;
	}

	IList<Vertex^>^ EnergyModel::ScaleFaceVertices(Face^ buildingFace, double scaleFactor)
	{
		Wire^ pApertureWire = buildingFace->ExternalBoundary;
		List<Vertex^>^ vertices = (List<Vertex^>^)pApertureWire->Vertices;
		vertices->Reverse();

		return ScaleVertices(vertices, scaleFactor);
	}

	IList<Vertex^>^ EnergyModel::ScaleVertices(IList<Vertex^>^ vertices, double scaleFactor)
	{
		List<Vertex^>^ scaledVertices = gcnew List<Vertex^>();
		double sqrtScaleFactor = Math::Sqrt(scaleFactor);
		Vertex^ centreVertex = GetCentreVertex(vertices);
		Autodesk::DesignScript::Geometry::Point^ faceCenterPoint =
			safe_cast<Autodesk::DesignScript::Geometry::Point^>(centreVertex->BasicGeometry);
		for each(Vertex^ aVertex in vertices)
		{
			Autodesk::DesignScript::Geometry::Point^ originalPoint =
				safe_cast<Autodesk::DesignScript::Geometry::Point^>(aVertex->BasicGeometry);
			Autodesk::DesignScript::Geometry::Vector^ originalPointAsVector = faceCenterPoint->AsVector();
			Autodesk::DesignScript::Geometry::Point^ translatedPoint = originalPoint->Subtract(originalPointAsVector);
			Autodesk::DesignScript::Geometry::Point^ scaledPoint = safe_cast<Autodesk::DesignScript::Geometry::Point^>(translatedPoint->Scale(sqrtScaleFactor, sqrtScaleFactor, sqrtScaleFactor));
			Autodesk::DesignScript::Geometry::Vector^ scaledPointAsVector = faceCenterPoint->AsVector();
			Autodesk::DesignScript::Geometry::Point^ reTranslatedPoint = scaledPoint->Add(scaledPointAsVector);
			scaledVertices->Add(safe_cast<Vertex^>(Topologic::Topology::ByGeometry(reTranslatedPoint, 0.001))); // tolerance does not matter as it's just a vertex

			delete reTranslatedPoint;
			delete scaledPointAsVector;
			delete scaledPoint;
			delete translatedPoint;
			delete originalPointAsVector;
			delete originalPoint;
		}

		delete faceCenterPoint;

		return scaledVertices;
	}

	Vertex^ EnergyModel::GetCentreVertex(IList<Vertex^>^ vertices)
	{
		Autodesk::DesignScript::Geometry::Point^ sumPoint = Autodesk::DesignScript::Geometry::Point::ByCoordinates(0, 0, 0);

		// assume vertices.count > 0
		if (vertices->Count < 3)
		{
			throw gcnew Exception("Invalid face");
		}

		for each(Vertex^ v in vertices)
		{
			Autodesk::DesignScript::Geometry::Point^ p =
				safe_cast<Autodesk::DesignScript::Geometry::Point^>(v->BasicGeometry);
			Autodesk::DesignScript::Geometry::Vector^ vector = p->AsVector();
			sumPoint = sumPoint->Add(vector);
			delete vector;
			delete p;
		}
		
		double scalingFactor = 1.0 / (double)vertices->Count;
		Autodesk::DesignScript::Geometry::Geometry^ scaledPoint = sumPoint->Scale(scalingFactor, scalingFactor, scalingFactor);
		Autodesk::DesignScript::Geometry::Point^ dynamoPoint = safe_cast<Autodesk::DesignScript::Geometry::Point^>(scaledPoint);
		Vertex^ vertex = safe_cast<Vertex^>(Topologic::Topology::ByGeometry(dynamoPoint, 0.001)); // tolerance does not matter as it's just a vertex
		delete dynamoPoint;
		delete sumPoint;
		return vertex;
	}

	OpenStudio::Point3dVector^ EnergyModel::GetFacePoints(Face^ buildingFace)
	{
		Wire^ buildingOuterWire = buildingFace->ExternalBoundary;
		IList<Vertex^>^ vertices = buildingOuterWire->Vertices;

		OpenStudio::Point3dVector^ osFacePoints = gcnew OpenStudio::Point3dVector();

		for each(Vertex^ v in vertices)
		{
			OpenStudio::Point3d^ osPoint = gcnew OpenStudio::Point3d(
				v->X,
				v->Y,
				v->Z);

			osFacePoints->Add(osPoint);
		}

		return osFacePoints;
	}

	bool EnergyModel::IsUnderground(Face^ buildingFace)
	{
		IList<Vertex^>^ vertices = buildingFace->Vertices;

		for each(Vertex^ aVertex in vertices)
		{
			if (aVertex->Z > 0.0)
			{
				return false;
			}
		}

		return true;
	}

	FaceType EnergyModel::CalculateFaceType(Face^ buildingFace, OpenStudio::Point3dVector^% facePoints, Cell^ buildingSpace, Autodesk::DesignScript::Geometry::Vector^ upVector)
	{
		FaceType faceType = FACE_WALL;
		IList<Face^>^ faces = (IList<Face^>^) Topologic::Utilities::FaceUtility::Triangulate(buildingFace, 0.01);
		if (faces->Count == 0)
		{
			throw gcnew Exception("Failed to triangulate a face.");
		}

		Face^ firstFace = faces[0];
		Vertex^ centerPoint = firstFace->CenterOfMass;
		Autodesk::DesignScript::Geometry::Point^ dynamoCenterPoint =
			safe_cast<Autodesk::DesignScript::Geometry::Point^>(centerPoint->BasicGeometry);

		IList<Vertex^>^ vertices = (IList<Vertex^>^)buildingFace->Vertices;
		Autodesk::DesignScript::Geometry::Point^ p1 =
			safe_cast<Autodesk::DesignScript::Geometry::Point^>(vertices[0]->BasicGeometry);
		Autodesk::DesignScript::Geometry::Vector^ p1AsVector = p1->AsVector();
		Autodesk::DesignScript::Geometry::Point^ p2 =
			safe_cast<Autodesk::DesignScript::Geometry::Point^>(vertices[1]->BasicGeometry);
		Autodesk::DesignScript::Geometry::Point^ p2Subtract = p2->Subtract(p1AsVector);
		Autodesk::DesignScript::Geometry::Vector^ p2SubtractAsVector = p2Subtract->AsVector();
		Autodesk::DesignScript::Geometry::Point^ p3 =
			safe_cast<Autodesk::DesignScript::Geometry::Point^>(vertices[2]->BasicGeometry);
		Autodesk::DesignScript::Geometry::Point^ p3Subtract = p3->Subtract(p1AsVector);
		Autodesk::DesignScript::Geometry::Vector^ p3SubtractAsVector = p3Subtract->AsVector();
		Autodesk::DesignScript::Geometry::Vector^ dir = p2SubtractAsVector->Cross(p3SubtractAsVector);
		Autodesk::DesignScript::Geometry::Vector^ faceNormal = dir->Normalized();
		double faceAngle = faceNormal->AngleWithVector(upVector);
		Autodesk::DesignScript::Geometry::Vector^ scaledFaceNormal = faceNormal->Scale(0.001, 0.001, 0.001);
		Autodesk::DesignScript::Geometry::Point^ pDynamoOffsetPoint =
			dynamic_cast<Autodesk::DesignScript::Geometry::Point^>(dynamoCenterPoint->Translate(scaledFaceNormal));

		Vertex^ pOffsetVertex = safe_cast<Vertex^>(Topologic::Topology::ByGeometry(pDynamoOffsetPoint, 0.001)); // tolerance does not matter as it's just a vertex

		if (faceAngle < 5.0 || faceAngle > 175.0)
		{
			bool isInside = Topologic::Utilities::CellUtility::Contains(buildingSpace, pOffsetVertex, true, 0.0001);
			// The offset vertex has to be false, so if isInside is true, reverse the face.

			if (isInside)
			{
				facePoints->Reverse();
				faceNormal = faceNormal->Reverse();
				faceAngle = faceNormal->AngleWithVector(upVector);
			}

			if (faceAngle < 5.0)
			{
				faceType = FACE_ROOFCEILING;
			}
			else if (faceAngle > 175.0)
			{
				faceType = FACE_FLOOR;
			}
		}

		delete dynamoCenterPoint;
		delete p1;
		delete p1AsVector;
		delete p2;
		delete p2Subtract;
		delete p2SubtractAsVector;
		delete p3;
		delete p3Subtract;
		delete p3SubtractAsVector;
		delete dir;
		delete faceNormal;
		delete scaledFaceNormal;
		delete pDynamoOffsetPoint;

		return faceType;
	}

	int EnergyModel::AdjacentCellCount(Face^ buildingFace)
	{
		return buildingFace->Cells->Count;
	}

	int EnergyModel::StoryNumber(Cell^ buildingCell, double buildingHeight, IList<double>^ floorLevels)
	{
		IList<double>^ floorLevelList = (IList<double>^) floorLevels;
		double volume = Utilities::CellUtility::Volume(buildingCell);
		Vertex^ centreOfMass = buildingCell->CenterOfMass;
		for (int i = 0; i < floorLevelList->Count - 1; ++i)
		{
			if (centreOfMass->Z > floorLevelList[i] && centreOfMass->Z < floorLevelList[i + 1])
			{
				return i;
			}
		}

		return 0;
	}

	bool EnergyModel::ExportTogbXML(EnergyModel^ energyModel, String ^ filePath)
	{
		if (filePath == nullptr)
		{
			throw gcnew Exception("The input filePath must not be null.");
		}

		if (energyModel == nullptr)
		{
			throw gcnew Exception("The input energy model is null.");
		}

		OpenStudio::Model^ osModel = energyModel->OsModel;
		OpenStudio::GbXMLForwardTranslator^ osForwardTranslator = gcnew OpenStudio::GbXMLForwardTranslator();
		OpenStudio::Path^ osPath = OpenStudio::OpenStudioUtilitiesCore::toPath(filePath);
		bool success = osForwardTranslator->modelToGbXML(osModel, osPath);
		OpenStudio::LogMessageVector^ osErrors = osForwardTranslator->errors();
		if (osErrors->Count > 0)
		{
			String^ errorMsg = gcnew String("Fails exporting an EnergyModel to GbXML with the following errors:\n");
			int i = 0;
			for each(auto osError in osErrors)
			{
				errorMsg += osError->logMessage();

				if (i < osErrors->Count - 1)
				{
					errorMsg += "\n";
				}
				++i;
			}
			throw gcnew Exception(errorMsg);
		}

		return true;
	}

	EnergyModel^ EnergyModel::ByImportedgbXML(String ^ filePath, double tolerance)
	{
		if (filePath == nullptr)
		{
			throw gcnew Exception("The input filePath must not be null.");
		}

		if (tolerance <= 0.0)
		{
			throw gcnew Exception("The tolerance must have a positive value.");
		}

		OpenStudio::GbXMLReverseTranslator^ osReverseTranslator = gcnew OpenStudio::GbXMLReverseTranslator();
		OpenStudio::Path^ osPath = OpenStudio::OpenStudioUtilitiesCore::toPath(filePath);
		OpenStudio::OptionalModel^ osOptionalModel = osReverseTranslator->loadModel(osPath);
		if (osOptionalModel->isNull())
		{
			throw gcnew Exception("The imported gbXML yields a null OpenStudio Model.");
		}
		OpenStudio::Model^ osModel = osOptionalModel->get();

		OpenStudio::LogMessageVector^ osErrors = osReverseTranslator->errors();
		if (osErrors->Count > 0)
		{
			String^ errorMsg = gcnew String("Fails importing an OpenStudio model from GbXML with the following errors:\n");
			int i = 0;
			for each(auto osError in osErrors)
			{
				errorMsg += osError->logMessage();

				if (i < osErrors->Count - 1)
				{
					errorMsg += "\n";
				}
				++i;
			}
			throw gcnew Exception(errorMsg);
		}


		OpenStudio::Building^ osBuilding = nullptr;
		List<Cell^>^ buildingCells = nullptr;
		Cluster^ shadingFaces = nullptr;
		OpenStudio::SpaceVector^ osSpaceVector = nullptr;
		ProcessOsModel(osModel, tolerance, osBuilding, buildingCells, shadingFaces, osSpaceVector);

		EnergyModel^ energyModel = gcnew EnergyModel(osModel, osBuilding, buildingCells, shadingFaces, osSpaceVector);

		return energyModel;
	}

	IList<int>^ EnergyModel::GetColor(double ratio)
	{
		double r = 0.0;
		double g = 0.0;
		double b = 0.0;

		double finalRatio = ratio;
		if (finalRatio < 0.0)
		{
			finalRatio = 0.0;
		}
		else if(finalRatio > 1.0)
		{
			finalRatio = 1.0;
		}

		if (finalRatio >= 0.0 && finalRatio <= 0.25)
		{
			r = 0.0;
			g = 4.0 * finalRatio;
			b = 1.0;
		}
		else if (finalRatio > 0.25 && finalRatio <= 0.5)
		{
			r = 0.0;
			g = 1.0;
			b = 1.0 - 4.0 * (finalRatio - 0.25);
		}
		else if (finalRatio > 0.5 && finalRatio <= 0.75)
		{
			r = 4.0*(finalRatio - 0.5);
			g = 1.0;
			b = 0.0;
		}
		else
		{
			r = 1.0;
			g = 1.0 - 4.0 * (finalRatio - 0.75);
			b = 0.0;
		}

		int rcom = (int) Math::Floor(Math::Max(Math::Min(Math::Floor(255.0 * r), 255.0), 0.0));
		int gcom = (int) Math::Floor(Math::Max(Math::Min(Math::Floor(255.0 * g), 255.0), 0.0));
		int bcom = (int) Math::Floor(Math::Max(Math::Min(Math::Floor(255.0 * b), 255.0), 0.0));
		List<int>^ rgb = gcnew List<int>();
		rgb->Add(rcom);
		rgb->Add(gcom);
		rgb->Add(bcom);
		return rgb;
	}

	String^ EnergyModel::BuildingName::get()
	{
		if (m_osBuilding == nullptr)
		{
			return "";
		}

		OpenStudio::OptionalString^ osName = m_osBuilding->name();
		return osName->get();
	}

	IList<Topologic::Cell^>^ EnergyModel::Topology::get()
	{
		return m_buildingCells;
	}

	EnergyModel::EnergyModel(OpenStudio::Model^ osModel, OpenStudio::Building^ osBuilding, IList<Topologic::Cell^>^ pBuildingCells, 
		Cluster^ shadingSurfaces, OpenStudio::SpaceVector^ osSpaces)
		: m_osModel(osModel)
		, m_osBuilding(osBuilding)
		, m_buildingCells(pBuildingCells)
		, m_osSpaceVector(osSpaces)
		, m_shadingSurfaces(shadingSurfaces)
	{

	}
}