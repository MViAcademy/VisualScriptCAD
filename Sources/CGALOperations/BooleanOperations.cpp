#include "BooleanOperations.hpp"
#include "TriangleUtils.hpp"
#include "IncludeGLM.hpp"
#include "BasicShapes.hpp"

#pragma warning (push)
#pragma warning (disable : 4456)
#pragma warning (disable : 4457)
#pragma warning (disable : 4458)
#pragma warning (disable : 4702)
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polyhedron_incremental_builder_3.h>
#include <CGAL/Polyhedron_3.h>
#include <CGAL/Cartesian_converter.h>
#pragma warning (pop)

typedef CGAL::Exact_predicates_exact_constructions_kernel	CGAL_Kernel;
typedef CGAL_Kernel::Point_3								CGAL_Point;
typedef CGAL::Surface_mesh<CGAL_Point>						CGAL_Mesh;
typedef CGAL::Polyhedron_3<CGAL_Kernel>						CGAL_Polyhedron;
typedef CGAL_Polyhedron::HalfedgeDS							CGAL_HalfedgeDS;

// https://stackoverflow.com/questions/53837772/cgal-convert-non-manifold-nef-polyhedron-3-to-triangle-mesh
// https://github.com/CGAL/cgal/blob/master/Polygon_mesh_processing/examples/Polygon_mesh_processing/corefinement_mesh_union_with_attributes.cpp

namespace CGALOperations
{

enum class NormalDirection
{
	Original,
	Reversed
};

class FaceId
{
public:
	FaceId () :
		mesh (nullptr),
		faceIndex (-1),
		normalDir (NormalDirection::Original)
	{
	}

	FaceId (const Modeler::Mesh* mesh, int faceIndex, NormalDirection normalDir) :
		mesh (mesh),
		faceIndex (faceIndex),
		normalDir (normalDir)
	{
	}

	const Modeler::Mesh*	mesh;
	int						faceIndex;
	NormalDirection			normalDir;
};

static void ConvertMeshToCGALMesh (const Modeler::Mesh& mesh, CGAL_Mesh& cgalMesh, NormalDirection normalDir, CGAL_Mesh::Property_map<CGAL_Mesh::Face_index, FaceId>& propertyMap)
{
	propertyMap = cgalMesh.add_property_map<CGAL_Mesh::Face_index, FaceId> ("faceid", FaceId ()).first;
		
	const Modeler::MeshGeometry& geometry = mesh.GetGeometry ();
	const glm::dmat4& transformation = mesh.GetTransformation ();
	geometry.EnumerateVertices (transformation, [&] (const glm::dvec3& vertex) {
		cgalMesh.add_vertex (CGAL_Point (vertex.x, vertex.y, vertex.z));
	});
	for (unsigned int triangleIndex = 0; triangleIndex < geometry.TriangleCount (); ++triangleIndex) {
		const Modeler::MeshTriangle& triangle = geometry.GetTriangle (triangleIndex);
		CGAL_Mesh::Face_index faceIndex = cgalMesh.add_face (
			CGAL_Mesh::Vertex_index (triangle.v1),
			CGAL_Mesh::Vertex_index (triangle.v2),
			CGAL_Mesh::Vertex_index (triangle.v3)
		);
		propertyMap[faceIndex] = FaceId (&mesh, triangleIndex, normalDir);
	}
}

static void ConvertCGALMeshToMesh (const CGAL_Mesh& cgalMesh, const CGAL_Mesh::Property_map<CGAL_Mesh::Face_index, FaceId>& propertyMap, Modeler::Mesh& mesh) 
{
	class MeshBuilder
	{
	public:
		MeshBuilder (Modeler::Mesh& resultMesh) :
			resultMesh (resultMesh)
		{
		}

		void AddVertex (double x, double y, double z)
		{
			resultMesh.AddVertex (x, y, z);
		}

		void AddTriangle (unsigned int v1, unsigned int v2, unsigned int v3, const FaceId& faceId)
		{
			const Modeler::Mesh& oldMesh = *faceId.mesh;
			const Modeler::MeshGeometry& oldGeometry = oldMesh.GetGeometry ();
			const Modeler::MeshTriangle& oldTriangle = oldGeometry.GetTriangle (faceId.faceIndex);

			const glm::dvec3& oldV1 = GetTransformedVertex (faceId.mesh, oldTriangle.v1);
			const glm::dvec3& oldV2 = GetTransformedVertex (faceId.mesh, oldTriangle.v2);
			const glm::dvec3& oldV3 = GetTransformedVertex (faceId.mesh, oldTriangle.v3);
			const glm::dvec3& oldN1 = GetTransformedNormal (faceId.mesh, oldTriangle.n1);
			const glm::dvec3& oldN2 = GetTransformedNormal (faceId.mesh, oldTriangle.n2);
			const glm::dvec3& oldN3 = GetTransformedNormal (faceId.mesh, oldTriangle.n3);
			
			const glm::dvec3& newV1 = resultMesh.GetGeometry ().GetVertex (v1);
			const glm::dvec3& newV2 = resultMesh.GetGeometry ().GetVertex (v2);
			const glm::dvec3& newV3 = resultMesh.GetGeometry ().GetVertex (v3);

			glm::dvec3 newN1 = glm::normalize (Geometry::BarycentricInterpolation (oldV1, oldV2, oldV3, oldN1, oldN2, oldN3, newV1));
			glm::dvec3 newN2 = glm::normalize (Geometry::BarycentricInterpolation (oldV1, oldV2, oldV3, oldN1, oldN2, oldN3, newV2));
			glm::dvec3 newN3 = glm::normalize (Geometry::BarycentricInterpolation (oldV1, oldV2, oldV3, oldN1, oldN2, oldN3, newV3));
			if (faceId.normalDir == NormalDirection::Reversed) {
				newN1 *= -1.0;
				newN2 *= -1.0;
				newN3 *= -1.0;
			}

			unsigned int n1 = resultMesh.AddNormal (newN1);
			unsigned int n2 = resultMesh.AddNormal (newN2);
			unsigned int n3 = resultMesh.AddNormal (newN3);

			Modeler::MaterialId materialId = GetMaterialId (faceId);
			resultMesh.AddTriangle (v1, v2, v3, n1, n2, n3, materialId);
		}

	private:
		const glm::dvec3& GetTransformedVertex (const Modeler::Mesh* mesh, unsigned int vertexId)
		{
			if (transformedVertexMap.find (mesh) == transformedVertexMap.end ()) {
				transformedVertexMap.insert ({ mesh, std::vector<glm::dvec3> () });
				std::vector<glm::dvec3>& vertices = transformedVertexMap.at (mesh);
				mesh->GetGeometry ().EnumerateVertices (mesh->GetTransformation (), [&] (const glm::dvec3& vertex) {
					vertices.push_back (vertex);
				});
			}
			return transformedVertexMap.at (mesh)[vertexId];
		}

		const glm::dvec3& GetTransformedNormal (const Modeler::Mesh* mesh, unsigned int normalId)
		{
			if (transformedNormalMap.find (mesh) == transformedNormalMap.end ()) {
				transformedNormalMap.insert ({ mesh, std::vector<glm::dvec3> () });
				std::vector<glm::dvec3>& normals = transformedNormalMap.at (mesh);
				mesh->GetGeometry ().EnumerateNormals (mesh->GetTransformation (), [&] (const glm::dvec3& normal) {
					normals.push_back (normal);
				});
			}
			return transformedNormalMap.at (mesh)[normalId];
		}

		Modeler::MaterialId GetMaterialId (const FaceId& faceId)
		{
			if (materialMap.find (faceId.mesh) == materialMap.end ()) {
				materialMap.insert ({ faceId.mesh, std::unordered_map<Modeler::MaterialId, Modeler::MaterialId> () });
			}
			std::unordered_map<Modeler::MaterialId, Modeler::MaterialId>& meshMap = materialMap.at (faceId.mesh);
			const Modeler::MeshMaterials& materials = faceId.mesh->GetMaterials ();
			Modeler::MaterialId oldMaterialId = materials.GetTriangleMaterial (faceId.faceIndex);
			if (meshMap.find (oldMaterialId) == meshMap.end ()) {
				Modeler::MaterialId newMaterialId = resultMesh.AddMaterial (materials.GetMaterial (oldMaterialId));
				meshMap.insert ({ oldMaterialId, newMaterialId });
			}
			return meshMap.at (oldMaterialId);
		}

		Modeler::Mesh& resultMesh;
		std::unordered_map<const Modeler::Mesh*, std::vector<glm::dvec3>> transformedVertexMap;
		std::unordered_map<const Modeler::Mesh*, std::vector<glm::dvec3>> transformedNormalMap;
		std::unordered_map<const Modeler::Mesh*, std::unordered_map<Modeler::MaterialId, Modeler::MaterialId>> materialMap;
	};

	MeshBuilder builder (mesh);
	for (CGAL_Mesh::Vertex_index vertIndex : cgalMesh.vertices ()) {
		const CGAL_Point& point = cgalMesh.point (vertIndex);
		builder.AddVertex (CGAL::to_double (point.x ()), CGAL::to_double (point.y ()), CGAL::to_double (point.z ()));
	}

	for (CGAL_Mesh::Face_index faceIndex : cgalMesh.faces ()) {
		CGAL_Mesh::Halfedge_index halfEdge = cgalMesh.halfedge (faceIndex);
		std::vector<CGAL_Mesh::Vertex_index> vertices;
		for (auto halfEdgeIndex : halfedges_around_face (halfEdge, cgalMesh)) {
			vertices.push_back (target (halfEdgeIndex, cgalMesh));
		}
		if (vertices.size () != 3) {
			throw std::logic_error ("a non-triangle face found");
		}
		FaceId faceId = propertyMap[faceIndex];
		if (faceId.mesh == nullptr || faceId.faceIndex == -1) {
			throw std::logic_error ("no parent face index found");
		}
		builder.AddTriangle ((unsigned int) vertices[0], (unsigned int) vertices[1], (unsigned int) vertices[2], faceId);
	}
}

class CGALMeshVisitor : public CGAL::Polygon_mesh_processing::Corefinement::Default_visitor<CGAL_Mesh>
{
public:
	CGALMeshVisitor () :
		faceId ()
	{

	}

	void before_subface_creations (CGAL_Mesh::Face_index splitFace, CGAL_Mesh& mesh)
	{
		faceId = properties[&mesh][splitFace];
	}

	void after_subface_created (CGAL_Mesh::Face_index newFace, CGAL_Mesh& mesh)
	{
		properties[&mesh][newFace] = faceId;
	}

	void after_face_copy (face_descriptor sourceFace, CGAL_Mesh& sourceMesh, face_descriptor targetFace, CGAL_Mesh& targetMesh)
	{
		properties[&targetMesh][targetFace] = properties[&sourceMesh][sourceFace];
	}

	FaceId faceId;
	std::unordered_map<const CGAL_Mesh*, CGAL_Mesh::Property_map<CGAL_Mesh::Face_index, FaceId>> properties;
};

class CGALMeshData
{
public:
	CGALMeshData ()
	{
		cgalMeshPropertyMap = cgalMesh.add_property_map<CGAL_Mesh::Face_index, FaceId> ("faceid", FaceId ()).first;
	}

	CGALMeshData (const Modeler::Mesh& mesh, NormalDirection normalDir)
	{
		ConvertMeshToCGALMesh (mesh, cgalMesh, normalDir, cgalMeshPropertyMap);
	}

	CGAL_Mesh& GetCGALMesh ()
	{
		return cgalMesh;
	}

	CGAL_Mesh::Property_map<CGAL_Mesh::Face_index, FaceId>& GetPropertyMap ()
	{
		return cgalMeshPropertyMap;
	}

private:
	CGAL_Mesh												cgalMesh;
	CGAL_Mesh::Property_map<CGAL_Mesh::Face_index, FaceId>	cgalMeshPropertyMap;
};

enum class BooleanOperation
{
	Difference,
	Intersection,
	Union
};

static bool MeshBooleanOperationWithCGALMesh (const Modeler::Mesh& aMesh, const Modeler::Mesh& bMesh, BooleanOperation operation, Modeler::Mesh& resultMesh)
{
	CGALMeshData aCGALMesh (aMesh, NormalDirection::Original);
	CGALMeshData bCGALMesh (bMesh, operation == BooleanOperation::Difference ? NormalDirection::Reversed : NormalDirection::Original);
	CGALMeshData resultCGALMesh;

	CGALMeshVisitor visitor;
	visitor.properties.insert ({ &aCGALMesh.GetCGALMesh (), aCGALMesh.GetPropertyMap () } );
	visitor.properties.insert ({ &bCGALMesh.GetCGALMesh (), bCGALMesh.GetPropertyMap () } );
	visitor.properties.insert ({ &resultCGALMesh.GetCGALMesh (), resultCGALMesh.GetPropertyMap () } );

	bool opResult = false;
	if (operation == BooleanOperation::Difference) {
		opResult = CGAL::Polygon_mesh_processing::corefine_and_compute_difference (aCGALMesh.GetCGALMesh (), bCGALMesh.GetCGALMesh (), resultCGALMesh.GetCGALMesh (), CGAL::Polygon_mesh_processing::parameters::visitor (visitor));
	} else if (operation == BooleanOperation::Intersection) {
		opResult = CGAL::Polygon_mesh_processing::corefine_and_compute_intersection (aCGALMesh.GetCGALMesh (), bCGALMesh.GetCGALMesh (), resultCGALMesh.GetCGALMesh (), CGAL::Polygon_mesh_processing::parameters::visitor (visitor));
	} else if (operation == BooleanOperation::Union) {
		opResult = CGAL::Polygon_mesh_processing::corefine_and_compute_union (aCGALMesh.GetCGALMesh (), bCGALMesh.GetCGALMesh (), resultCGALMesh.GetCGALMesh (), CGAL::Polygon_mesh_processing::parameters::visitor (visitor));
	} else {
		throw std::exception ("invalid boolean operation");
	}

	if (!opResult) {
		return false;
	}

	ConvertCGALMeshToMesh (resultCGALMesh.GetCGALMesh (), resultCGALMesh.GetPropertyMap (), resultMesh);
	return true;
}

static bool MeshBooleanOperation (const Modeler::Mesh& aMesh, const Modeler::Mesh& bMesh, BooleanOperation operation, Modeler::Mesh& resultMesh)
{
	// use the same rounding for mesh generation as for operation
	// also workaround a CGAL bug of not resetting the rounding value sometimes
	CGAL::Protect_FPU_rounding<true> protect (CGAL_FE_UPWARD);

	bool success = false;
	try {
		success = MeshBooleanOperationWithCGALMesh (aMesh, bMesh, operation, resultMesh);
	} catch (...) {
		success = false;
	}

	return success;
}

static Modeler::ShapePtr ShapeBooleanOperation (const Modeler::ShapeConstPtr& aShape, const Modeler::ShapeConstPtr& bShape, BooleanOperation operation)
{
	Modeler::Mesh aMesh = aShape->GenerateMesh ();
	Modeler::Mesh bMesh = bShape->GenerateMesh ();
	Modeler::Mesh resultMesh;
	if (!MeshBooleanOperation (aMesh, bMesh, operation, resultMesh)) {
		return nullptr;
	}
	return std::shared_ptr<Modeler::MeshShape> (new Modeler::MeshShape (glm::dmat4 (1.0), resultMesh));
}

bool MeshDifference (const Modeler::Mesh& aMesh, const Modeler::Mesh& bMesh, Modeler::Mesh& resultMesh)
{
	return MeshBooleanOperation (aMesh, bMesh, BooleanOperation::Difference, resultMesh);
}

bool MeshIntersection (const Modeler::Mesh& aMesh, const Modeler::Mesh& bMesh, Modeler::Mesh& resultMesh)
{
	return MeshBooleanOperation (aMesh, bMesh, BooleanOperation::Intersection, resultMesh);
}

bool MeshUnion (const Modeler::Mesh& aMesh, const Modeler::Mesh& bMesh, Modeler::Mesh& resultMesh)
{
	return MeshBooleanOperation (aMesh, bMesh, BooleanOperation::Union, resultMesh);
}

bool MeshUnion (const std::vector<Modeler::Mesh>& meshes, Modeler::Mesh& resultMesh)
{
	if (meshes.empty ()) {
		return false;
	}
	resultMesh = meshes[0];
	for (size_t i = 1; i < meshes.size (); i++) {
		Modeler::Mesh aMesh = resultMesh;
		const Modeler::Mesh& bMesh = meshes[i];
		resultMesh.Clear ();
		if (!MeshUnion (aMesh, bMesh, resultMesh)) {
			resultMesh.Clear ();
			return false;
		}
	}
	return true;
}

Modeler::ShapePtr ShapeDifference (const Modeler::ShapeConstPtr& aShape, const Modeler::ShapeConstPtr& bShape)
{
	return ShapeBooleanOperation (aShape, bShape, BooleanOperation::Difference);
}

Modeler::ShapePtr ShapeIntersection (const Modeler::ShapeConstPtr& aShape, const Modeler::ShapeConstPtr& bShape)
{
	return ShapeBooleanOperation (aShape, bShape, BooleanOperation::Intersection);
}

Modeler::ShapePtr ShapeUnion (const Modeler::ShapeConstPtr& aShape, const Modeler::ShapeConstPtr& bShape)
{
	return ShapeBooleanOperation (aShape, bShape, BooleanOperation::Union);
}

Modeler::ShapePtr ShapeUnion (const std::vector<Modeler::ShapeConstPtr>& shapes)
{
	//if (shapes.empty ()) {
	//	return nullptr;
	//}
	//if (shapes.size () == 1) {
	//	return shapes[0]->Clone ();
	//}

	std::vector<Modeler::Mesh> meshes;
	for (const Modeler::ShapeConstPtr& shape : shapes) {
		meshes.push_back (shape->GenerateMesh ());
	}
	Modeler::Mesh resultMesh;
	if (!MeshUnion (meshes, resultMesh)) {
		return nullptr;
	}
	return std::shared_ptr<Modeler::MeshShape> (new Modeler::MeshShape (glm::dmat4 (1.0), resultMesh));
}

}
