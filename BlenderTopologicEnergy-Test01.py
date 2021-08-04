import sys
sys.path.append("C:/ProgramData/Anaconda3/envs/blender293/Lib/site-packages")
import openstudio

import topologic
import cppyy

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

osmFile = openstudio.openstudioutilitiescore.toPath("D:/OneDriveCardiffUniversity\OneDrive - Cardiff University/TopologicEnergy-Files/TinkerBuilding.osm")
m = openstudio.model.Model_load(osmFile).get()
building = m.getBuilding()
spaces = m.getSpaces()
vertexIndex = 0
cells = cppyy.gbl.std.list[topologic.Topology.Ptr]()
for aSpace in spaces:
    spaceFaces = cppyy.gbl.std.list[topologic.Face.Ptr]()
    surfaces = aSpace.surfaces()
    for aSurface in surfaces:
        surfaceEdges = cppyy.gbl.std.list[topologic.Edge.Ptr]()
        surfaceIndices = cppyy.gbl.std.list[int]()
        surfaceVertices = aSurface.vertices()
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
        spaceFaces.push_back(surfaceFace)
    spaceCell = topologic.Cell.ByFaces(spaceFaces)
    cells.push_back(spaceCell)

cluster = topologic.Cluster.ByTopologies(cells)
brepFile = "D:/OneDriveCardiffUniversity\OneDrive - Cardiff University/TopologicEnergy-Files/TinkerBuildingExport.brep"
exportToBREP([cluster], brepFile, True)
