import sys
sys.path.append("C:/ProgramData/Anaconda3/envs/blender293/Lib/site-packages")
import openstudio
import topologic
import cppyy
import datetime

def datetimeStamp():
    now = datetime.utcnow()
    return now.strftime("%Y-%m-%d-%H-%M-%S")

# Helper to load a model in one line
def osload(path):
    translator = openstudio.osversion.VersionTranslator()
    osmFile = openstudio.openstudioutilitiescore.toPath(path)
    model = translator.loadModel(osmFile)
    if model.isNull():
        raise Exception("Path is not a valid path to an OpenStudio Model")
        return None
    else:
        model = model.get()
    return model

def exportToBREP(topologyList, filepath, overwrite):
    # Make sure the file extension is .BREP
    ext = filepath[len(filepath)-5:len(filepath)]
    if ext.lower() != ".brep":
        filepath = filepath+".brep"
    f = None
    try:
        if overwrite == True:
            f = open(filepath, "w")
        else:
            f = open(filepath, "x") # Try to create a new File
    except:
        raise Exception("Error: Could not create a new file at the following location: "+filepath)
    if (f):
        if len(topologyList) > 1:
            stl_top = cppyy.gbl.std.list[topologic.Topology.Ptr]()
            for aTopology in topologyList:
                stl_top.push_back(aTopology)
            cluster = topologic.Cluster.ByTopologies(stl_top)
            topString = str(cluster.String())
        else:
            topString = str(topologyList[0].String())
        f.write(topString)
        f.close()
        return True
    return False

def surfaceToFace(surface):
    surfaceEdges = cppyy.gbl.std.list[topologic.Edge.Ptr]()
    surfaceIndices = cppyy.gbl.std.list[int]()
    surfaceVertices = surface.vertices()
    firstVertexIndex = vertexIndex
    for i in range(len(surfaceVertices)-1):
        sv = topologic.Vertex.ByCoordinates(surfaceVertices[i].x(), surfaceVertices[i].y(), surfaceVertices[i].z())
        ev = topologic.Vertex.ByCoordinates(surfaceVertices[i+1].x(), surfaceVertices[i+1].y(), surfaceVertices[i+1].z())
        edge = topologic.Edge.ByStartVertexEndVertex(sv, ev)
        surfaceEdges.push_back(edge)
    sv = topologic.Vertex.ByCoordinates(surfaceVertices[len(surfaceVertices)-1].x(), surfaceVertices[len(surfaceVertices)-1].y(), surfaceVertices[len(surfaceVertices)-1].z())
    ev = topologic.Vertex.ByCoordinates(surfaceVertices[0].x(), surfaceVertices[0].y(), surfaceVertices[0].z())
    edge = topologic.Edge.ByStartVertexEndVertex(sv, ev)
    surfaceEdges.push_back(edge)
    surfaceWire = topologic.Wire.ByEdges(surfaceEdges)
    internalBoundaries = cppyy.gbl.std.list[topologic.Wire.Ptr]()
    surfaceFace = topologic.Face.ByExternalInternalBoundaries(surfaceWire, internalBoundaries)
    return surfaceFace

def doubleValueFromQuery(sqlFile, EPReportName, EPReportForString, EPTableName, EPColumnName, EPRowName, EPUnits):
    doubleValue = 0.0
    query = "SELECT Value FROM tabulardatawithstrings WHERE ReportName='" + EPReportName + "' AND ReportForString='" + EPReportForString + "' AND TableName = '" + EPTableName + "' AND RowName = '" + EPRowName + "' AND ColumnName= '" + EPColumnName + "' AND Units='" + EPUnits + "'";
    osOptionalDoubleValue = sqlFile.execAndReturnFirstDouble(query)
    if (osOptionalDoubleValue.is_initialized()):
        doubleValue = osOptionalDoubleValue.get()
    else:
        raise Exception("Fails to get a double value from the SQL file.")
    return doubleValue;


osmFilePath = "C:/Users/wassi/osmFiles/ASHRAESecondarySchool.osm"
#osmFilePath = "C:/Users/wassi/fake.osm"
#sqlFile = openstudio.SqlFile(openstudio.openstudioutilitiescore.toPath(r"C:\Users\wassi\OneDrive - Cardiff University\TopologicEnergy-Output\TopologicEnergy_2020-05-06_13-27-15-141\run\eplusout.sql"))
#m = openstudio.model.Model_load(osmFile).get()
m = osload(osmFilePath)
if m:
    #m.setSqlFile(sqlFile)
    #EPReportName = "HVACSizingSummary"
    #EPReportForString = "Entire Facility"
    #EPTableName = "Zone Sensible Cooling"
    #EPColumnName = "Calculated Design Load"
    #EPUnits = "W"
    #EPRowName = "STORY_1_SPACE_1_THERMAL_ZONE"
    building = m.getBuilding()
    spaces = m.getSpaces()
    vertexIndex = 0
    apertures = cppyy.gbl.std.list[topologic.Topology.Ptr]()
    topologies = cppyy.gbl.std.list[topologic.Topology.Ptr]()
    for aSpace in spaces:
        osTransformation = aSpace.transformation()
        osTranslation = osTransformation.translation()
        osMatrix = osTransformation.rotationMatrix()
        rotation11 = osMatrix[0, 0]
        rotation12 = osMatrix[0, 1]
        rotation13 = osMatrix[0, 2]
        rotation21 = osMatrix[1, 0]
        rotation22 = osMatrix[1, 1]
        rotation23 = osMatrix[1, 2]
        rotation31 = osMatrix[2, 0]
        rotation32 = osMatrix[2, 1]
        rotation33 = osMatrix[2, 2]
        spaceFaces = cppyy.gbl.std.list[topologic.Face.Ptr]()
        surfaces = aSpace.surfaces()
        for aSurface in surfaces:
            aFace = surfaceToFace(aSurface)
            aFace = topologic.TopologyUtility.Transform(aFace, osTranslation.x(), osTranslation.y(), osTranslation.z(), rotation11, rotation12, rotation13, rotation21, rotation22, rotation23, rotation31, rotation32, rotation33)
            aFace.__class__ = topologic.Face
            spaceFaces.push_back(aFace)
            subSurfaces = aSurface.subSurfaces()
            for aSubSurface in subSurfaces:
                aperture = surfaceToFace(aSubSurface)
                aperture = topologic.TopologyUtility.Transform(aperture, osTranslation.x(), osTranslation.y(), osTranslation.z(), rotation11, rotation12, rotation13, rotation21, rotation22, rotation23, rotation31, rotation32, rotation33)
                apertures.push_back(aperture)
                context = topologic.Context.ByTopologyParameters(aFace, 0.5, 0.5, 0.5)
                _ = topologic.Aperture.ByTopologyContext(aperture, context)
        spaceCell = topologic.Cell.ByFaces(spaceFaces)
        topologies.push_back(spaceCell)
        #EPRowName = aSpace.nameString()+"_THERMAL_ZONE"
        #print(EPRowName)
        #doubleValue = doubleValueFromQuery(sqlFile, EPReportName, EPReportForString, EPTableName, EPColumnName, EPRowName, EPUnits)
        #print(doubleValue)
    cellsCluster = topologic.Cluster.ByTopologies(topologies)
    aperturesCluster = topologic.Cluster.ByTopologies(apertures)
    brepFile = r"C:\Users\wassi\osmFiles\ASHRAESecondarySchool.brep"
    exportToBREP([cellsCluster], brepFile, True)
    brepFile = r"C:\Users\wassi\osmFiles\ASHRAESecondarySchoolApertures.brep"
    exportToBREP([aperturesCluster], brepFile, True)
