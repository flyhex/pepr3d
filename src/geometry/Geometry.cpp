#include "geometry/Geometry.h"
#include "GeometryUtils.h"
#include "tools/Brush.h"

#include <CGAL/Algebraic_kernel_for_spheres_2_3.h>
#include <CGAL/Sphere_3.h>
#include <CGAL/Spherical_kernel_3.h>
#include <CGAL/Spherical_kernel_intersections.h>
#include <set>

namespace pepr3d {

/* -------------------- Commands -------------------- */

Geometry::GeometryState Geometry::saveState() const {
    // Save only necessary data to keep snapshot size low
    return GeometryState{mTriangles, mTriangleDetails, ColorManager::ColorMap(mColorManager.getColorMap())};
}

void Geometry::loadState(const GeometryState& state) {
    mTriangles = state.triangles;
    mTriangleDetails = state.triangleDetails;

    mColorManager.replaceColors(state.colorMap.begin(), state.colorMap.end());
    assert(!mColorManager.empty());

    // TODO Improve: Can we reuse something?

    // Set opengl state to dirty so it gets updated eventually
    // Note: Updating straight away would hide this change from ModelView
    mOgl.isDirty = true;

    // Tree needs to be rebuilt to match loaded triangles
    mTree->rebuild(mTriangles.begin(), mTriangles.end());
    assert(mTree->size() == mTriangles.size());
}

/* -------------------- Mesh loading -------------------- */

void Geometry::loadNewGeometry(const std::string& fileName, ::ThreadPool& threadPool) {
    // Reset progress
    mProgress->resetLoad();

    /// Load into mTriangles
    ModelImporter modelImporter(fileName, mProgress.get(), threadPool);  // only first mesh [0]

    if(modelImporter.isModelLoaded()) {
        mTriangles = modelImporter.getTriangles();

        /// Async build the polyhedron data structure
        auto buildPolyhedronFuture = threadPool.enqueue([&modelImporter, this]() {
            mPolyhedronData.vertices.clear();
            mPolyhedronData.indices.clear();
            mPolyhedronData.vertices = modelImporter.getVertexBuffer();
            mPolyhedronData.indices = modelImporter.getIndexBuffer();
            buildPolyhedron();
        });

        /// Async build the AABB tree
        auto buildTreeFuture = threadPool.enqueue([this]() {
            mProgress->aabbTreePercentage = 0.0f;

            mTree->rebuild(mTriangles.cbegin(), mTriangles.cend());
            assert(mTree->size() == mTriangles.size());

            /// Get the new bounding box
            if(!mTree->empty()) {
                mBoundingBox = std::make_unique<BoundingBox>(mTree->bbox());
            }

            mProgress->aabbTreePercentage = 1.0f;
        });

        mProgress->buffersPercentage = 0.0f;

        /// Generate new vertex buffer
        generateVertexBuffer();
        mProgress->buffersPercentage = 0.25f;

        /// Generate new index buffer
        generateIndexBuffer();
        mProgress->buffersPercentage = 0.50f;

        /// Generate new color buffer from triangle color data
        generateColorBuffer();
        mProgress->buffersPercentage = 0.75f;

        /// Generate new normal buffer, copying the triangle normal to each vertex
        generateNormalBuffer();
        mProgress->buffersPercentage = 1.0f;

        /// Wait for building the polyhedron and tree
        buildTreeFuture.get();
        buildPolyhedronFuture.get();

        /// Get the generated color palette of the model, replace the current one
        mColorManager = modelImporter.getColorManager();
        assert(!mColorManager.empty());
    } else {
        CI_LOG_E("Model not loaded --> write out message for user");
    }
}

void Geometry::exportGeometry(const std::string filePath, const std::string fileName, const std::string fileType) {
    // Reset progress
    mProgress->resetSave();

    ModelExporter modelExporter(mTriangles, filePath, fileName, fileType, mProgress.get());
}

void Geometry::generateVertexBuffer() {
    mOgl.vertexBuffer.clear();
    mOgl.vertexBuffer.reserve(3 * mTriangles.size());

    for(size_t idx = 0; idx < mTriangles.size(); ++idx) {
        const auto& triangle = mTriangles[idx];

        if(isSimpleTriangle(idx)) {
            mOgl.vertexBuffer.push_back(triangle.getVertex(0));
            mOgl.vertexBuffer.push_back(triangle.getVertex(1));
            mOgl.vertexBuffer.push_back(triangle.getVertex(2));
        } else {
            // Pass dummy triangle to keep triangleIdx consistent with array position
            mOgl.vertexBuffer.push_back(glm::vec3{0, 0, 0});
            mOgl.vertexBuffer.push_back(glm::vec3{0, 0, 0});
            mOgl.vertexBuffer.push_back(glm::vec3{0, 0, 0});
        }
    }

    for(auto& it : mTriangleDetails) {
        const auto& detailTriangles = it.second.getTriangles();

        for(const auto& triangle : detailTriangles) {
            mOgl.vertexBuffer.push_back(triangle.getVertex(0));
            mOgl.vertexBuffer.push_back(triangle.getVertex(1));
            mOgl.vertexBuffer.push_back(triangle.getVertex(2));
        }
    }
}

void Geometry::generateIndexBuffer() {
    mOgl.indexBuffer.clear();
    mOgl.indexBuffer.reserve(mOgl.vertexBuffer.size());

    for(uint32_t i = 0; i < mOgl.vertexBuffer.size(); ++i) {
        mOgl.indexBuffer.push_back(i);
    }
}

void Geometry::generateColorBuffer() {
    mOgl.colorBuffer.clear();
    mOgl.colorBuffer.reserve(mOgl.vertexBuffer.size());

    for(size_t idx = 0; idx < mTriangles.size(); ++idx) {
        const auto& triangle = mTriangles[idx];
        const ColorIndex triColorIndex = static_cast<ColorIndex>(triangle.getColor());
        mOgl.colorBuffer.push_back(triColorIndex);
        mOgl.colorBuffer.push_back(triColorIndex);
        mOgl.colorBuffer.push_back(triColorIndex);
    }

    for(auto& it : mTriangleDetails) {
        const auto& detailTriangles = it.second.getTriangles();

        for(const auto& triangle : detailTriangles) {
            const ColorIndex triColorIndex = static_cast<ColorIndex>(triangle.getColor());
            mOgl.colorBuffer.push_back(triColorIndex);
            mOgl.colorBuffer.push_back(triColorIndex);
            mOgl.colorBuffer.push_back(triColorIndex);
        }
    }

    assert(mOgl.colorBuffer.size() == mOgl.vertexBuffer.size());
}

void Geometry::generateNormalBuffer() {
    mOgl.normalBuffer.clear();
    mOgl.normalBuffer.reserve(mOgl.vertexBuffer.size());
    for(size_t idx = 0; idx < mTriangles.size(); ++idx) {
        const auto& triangle = mTriangles[idx];
        mOgl.normalBuffer.push_back(triangle.getNormal());
        mOgl.normalBuffer.push_back(triangle.getNormal());
        mOgl.normalBuffer.push_back(triangle.getNormal());
    }

    for(auto& it : mTriangleDetails) {
        const auto& detailTriangles = it.second.getTriangles();
        glm::vec3 normal = it.second.getOriginal().getNormal();

        for(const auto& triangle : detailTriangles) {
            mOgl.normalBuffer.push_back(normal);
            mOgl.normalBuffer.push_back(normal);
            mOgl.normalBuffer.push_back(normal);
        }
    }
    assert(mOgl.normalBuffer.size() == mOgl.vertexBuffer.size());
}

void Geometry::generateHighlightBuffer() {
    std::set<size_t>& paintSet = mAreaHighlight.triangles;
    const BrushSettings& settings = mAreaHighlight.settings;

    mOgl.highlightMask.clear();
    // Mark all triangles with attribute assigned to vertex
    mOgl.highlightMask.reserve(mTriangles.size() * 3);
    for(size_t triangleIdx = 0; triangleIdx < mTriangles.size(); triangleIdx++) {
        // Fill 3 vertices of a triangle
        if(settings.continuous && paintSet.find(triangleIdx) == paintSet.end()) {
            mOgl.highlightMask.emplace_back(0);
            mOgl.highlightMask.emplace_back(0);
            mOgl.highlightMask.emplace_back(0);

        } else {
            mOgl.highlightMask.emplace_back(1);
            mOgl.highlightMask.emplace_back(1);
            mOgl.highlightMask.emplace_back(1);
        }
    }

    // If the original triangle has highlight enabled also enable for detail
    for(auto& it : mTriangleDetails) {
        const size_t triangleIdx = it.first;
        const auto& detailTriangles = it.second.getTriangles();

        const bool enableHighlight = !settings.continuous || paintSet.find(triangleIdx) != paintSet.end();

        for(const auto& triangle : detailTriangles) {
            if(enableHighlight) {
                mOgl.highlightMask.emplace_back(1);
                mOgl.highlightMask.emplace_back(1);
                mOgl.highlightMask.emplace_back(1);
            } else {
                mOgl.highlightMask.emplace_back(0);
                mOgl.highlightMask.emplace_back(0);
                mOgl.highlightMask.emplace_back(0);
            }
        }
    }

    assert(mOgl.highlightMask.size() == mOgl.vertexBuffer.size());

    mOgl.info.didHighlightUpdate = true;
}

/* -------------------- Tool support -------------------- */

std::optional<size_t> Geometry::intersectMesh(const ci::Ray& ray) const {
    if(mTree->empty()) {
        return {};
    }

    const glm::vec3 source = ray.getOrigin();
    const glm::vec3 direction = ray.getDirection();

    const pepr3d::Geometry::Ray rayQuery(pepr3d::DataTriangle::Point(source.x, source.y, source.z),
                                         pepr3d::Geometry::Direction(direction.x, direction.y, direction.z));

    // Find the two intersection parameters - place and triangle
    Ray_intersection intersection = mTree->first_intersection(rayQuery);
    if(intersection) {
        // The intersected triangle
        if(boost::get<DataTriangleAABBPrimitive::Id>(intersection->second) != mTriangles.end()) {
            const DataTriangleAABBPrimitive::Id intersectedTriIter =
                boost::get<DataTriangleAABBPrimitive::Id>(intersection->second);
            assert(intersectedTriIter != mTriangles.end());
            const size_t retValue = intersectedTriIter - mTriangles.begin();
            assert(retValue < mTriangles.size());
            return retValue;  // convert the iterator into an index
        }
    }

    /// No intersection detected.
    return {};
}

std::optional<size_t> Geometry::intersectMesh(const ci::Ray& ray, glm::vec3& outPos) const {
    auto intersection = intersectMesh(ray);

    if(intersection) {
        // Calculate intersection point of the ray with the triangle
        const DataTriangle& tri = getTriangle(*intersection);

        auto intersectionPoint = GeometryUtils::triangleRayIntersection(tri, ray);

        if(intersectionPoint)
            outPos = *intersectionPoint;
    }

    return intersection;
}

std::vector<size_t> Geometry::getTrianglesUnderBrush(const glm::vec3& originPoint, const glm::vec3& insideDirection,
                                                     size_t startTriangle, const struct BrushSettings& settings) {
    const double sizeSquared = settings.size * settings.size;

    /// Stop when the trinagle has no intersection with the area highlight
    auto stoppingCriterionSingleTri = [this, originPoint, sizeSquared, insideDirection, startTriangle,
                                       settings](const size_t triId) -> bool {
        // Always accept the first triangle
        if(triId == startTriangle)
            return true;

        const auto& tri = getTriangle(triId);
        const auto a = tri.getVertex(0);
        const auto b = tri.getVertex(1);
        const auto c = tri.getVertex(2);

        if(!settings.paintBackfaces && glm::dot(tri.getNormal(), insideDirection) > 0.f)
            return false;  // stop on triangles facing away from the ray

        // If any side has intersection with the brush keep the triangle
        if(GeometryUtils::segmentPointDistanceSquared(a, b, originPoint) < sizeSquared)
            return true;
        if(GeometryUtils::segmentPointDistanceSquared(b, c, originPoint) < sizeSquared)
            return true;
        if(GeometryUtils::segmentPointDistanceSquared(c, a, originPoint) < sizeSquared)
            return true;

        return false;
    };

    const auto stoppingCriterion = [&stoppingCriterionSingleTri, this](const size_t a, const size_t b) -> bool {
        return stoppingCriterionSingleTri(a) && stoppingCriterionSingleTri(b);
    };

    return bucket(startTriangle, stoppingCriterion);
}

void Geometry::highlightArea(const ci::Ray& ray, const BrushSettings& settings) {
    const glm::vec3 source = ray.getOrigin();
    const glm::vec3 rayDirection = ray.getDirection();

    glm::vec3 intersectionPoint{};
    auto intersectedTri = intersectMesh(ray, intersectionPoint);
    if(intersectedTri) {
        std::vector<size_t> trianglesToPaint;

        if(settings.continuous) {
            trianglesToPaint = getTrianglesUnderBrush(intersectionPoint, rayDirection, *intersectedTri, settings);
        }

        mAreaHighlight.triangles = std::set<size_t>(trianglesToPaint.begin(), trianglesToPaint.end());
        trianglesToPaint.clear();
        mAreaHighlight.settings = settings;
        mAreaHighlight.size = settings.size;
        mAreaHighlight.origin = intersectionPoint;
        mAreaHighlight.direction = ray.getDirection();
        mAreaHighlight.enabled = true;
        mAreaHighlight.dirty = true;

        // Generate highlight buffer only if our openGlBuffers are valid
        // Otherwise delay until everything is generated again
        if(!mOgl.isDirty) {
            generateHighlightBuffer();
        }

        // TODO: Improvement: Try to avoid doing all this if we are highlighting the same triangle with the same
        // size
    } else {
        mAreaHighlight.enabled = false;
    }
}

void Geometry::paintArea(const ci::Ray& ray, const BrushSettings& settings) {
    glm::vec3 intersectionPoint{};
    auto intersectedTri = intersectMesh(ray, intersectionPoint);

    if(!intersectedTri) {
        return;
    }

    const auto trisInBrush = getTrianglesUnderBrush(intersectionPoint, ray.getDirection(), *intersectedTri, settings);

    for(const size_t triangleIdx : trisInBrush) {
        const auto& cgalTri = getTriangle(triangleIdx).getTri();

        if(GeometryUtils::isFullyInsideASphere(cgalTri, intersectionPoint, settings.size)) {
            // Triangles fully inside are colored whole
            setTriangleColor(triangleIdx, settings.color);
        } else {
            if(settings.respectOriginalTriangles) {
                if(settings.paintOuterRing) {
                    setTriangleColor(triangleIdx, settings.color);
                }

            } else {
                // Do not paint triangles that are already the same color
                if(!isSimpleTriangle(triangleIdx) || getTriangle(triangleIdx).getColor() != settings.color) {
                    updateTriangleDetail(triangleIdx, intersectionPoint, settings);
                }
            }
        }
    }

    mOgl.isDirty = true;
}

TriangleDetail* Geometry::createTriangleDetail(size_t triangleIdx) {
    auto result =
        mTriangleDetails.emplace(triangleIdx, TriangleDetail(getTriangle(triangleIdx), getTriangleColor(triangleIdx)));

    return &(result.first->second);
}

void Geometry::updateTriangleDetail(size_t triangleIdx, const glm::vec3& brushOrigin,
                                    const struct BrushSettings& settings) {
    using Point = DataTriangle::K::Point_3;
    using Point2D = DataTriangle::K::Point_2;
    using Sphere = DataTriangle::K::Sphere_3;
    using Plane = DataTriangle::K::Plane_3;
    using Triangle = DataTriangle::Triangle;

    const Triangle& tri = getTriangle(triangleIdx).getTri();
    const Plane triPlane = tri.supporting_plane();

    const Sphere brushShape(Point(brushOrigin.x, brushOrigin.y, brushOrigin.z), settings.size * settings.size);
    getTriangleDetail(triangleIdx)->paintSphere(brushShape, settings.color);
}

void Geometry::removeTriangleDetail(const size_t triangleIndex) {
    mOgl.isDirty = true;
    mTriangleDetails.erase(triangleIndex);
}

void Geometry::setTriangleColor(const size_t triangleIndex, const size_t newColor) {
    if(isSimpleTriangle(triangleIndex)) {
        // Change it in the buffer
        // Color buffer has 1 ColorA for each vertex, each triangle has 3 vertices
        const size_t vertexPosition = triangleIndex * 3;

        // Change all vertices of the triangle to the same new color
        assert(vertexPosition + 2 < mOgl.colorBuffer.size());

        ColorIndex newColorIndex = static_cast<ColorIndex>(newColor);
        mOgl.colorBuffer[vertexPosition] = newColorIndex;
        mOgl.colorBuffer[vertexPosition + 1] = newColorIndex;
        mOgl.colorBuffer[vertexPosition + 2] = newColorIndex;
        mOgl.info.didColorUpdate = true;
    } else {
        removeTriangleDetail(triangleIndex);
    }

    /// Change it in the triangle soup
    assert(triangleIndex < mTriangles.size());
    mTriangles[triangleIndex].setColor(newColor);
}

void Geometry::buildPolyhedron() {
    mProgress->polyhedronPercentage = 0.0f;
    mPolyhedronData.mMesh.clear();
    mPolyhedronData.mMesh.remove_property_map(mPolyhedronData.mIdMap);
    mPolyhedronData.mMesh.remove_property_map(mPolyhedronData.sdf_property_map);
    mPolyhedronData.isSdfComputed = false;
    mPolyhedronData.valid = false;
    mPolyhedronData.mFaceDescs.clear();

    std::vector<PolyhedronData::vertex_descriptor> vertDescs;
    vertDescs.reserve(mPolyhedronData.vertices.size());
    mPolyhedronData.mMesh.reserve(static_cast<PolyhedronData::Mesh::size_type>(mPolyhedronData.vertices.size()),
                                  static_cast<PolyhedronData::Mesh::size_type>(mPolyhedronData.indices.size() * 3 / 2),
                                  static_cast<PolyhedronData::Mesh::size_type>(mPolyhedronData.indices.size()));

    for(const auto& vertex : mPolyhedronData.vertices) {
        PolyhedronData::vertex_descriptor v =
            mPolyhedronData.mMesh.add_vertex(DataTriangle::Point(vertex.x, vertex.y, vertex.z));
        vertDescs.push_back(v);
    }

    mPolyhedronData.mFaceDescs.reserve(mPolyhedronData.indices.size());
    bool meshHasNull = false;
    for(const auto& tri : mPolyhedronData.indices) {
        auto f = mPolyhedronData.mMesh.add_face(vertDescs[tri[0]], vertDescs[tri[1]], vertDescs[tri[2]]);
        if(f == PolyhedronData::Mesh::null_face()) {
            // Adding a non-valid face, the model is wrong and we stop.
            meshHasNull = true;
            break;
        } else {
            assert(f != PolyhedronData::Mesh::null_face());
            mPolyhedronData.mFaceDescs.push_back(f);
        }
    }
    // If the build failed, clear, set invalid and exit;
    if(meshHasNull) {
        mPolyhedronData.mMesh.clear();
        mPolyhedronData.valid = false;
        mProgress->polyhedronPercentage = -1.0f;
        return;
    }

    bool created;
    boost::tie(mPolyhedronData.mIdMap, created) =
        mPolyhedronData.mMesh.add_property_map<PolyhedronData::face_descriptor, size_t>(
            "f:idOfEachTriangle", std::numeric_limits<size_t>::max() - 1);
    assert(created);

    // Add the IDs
    size_t i = 0;
    for(const PolyhedronData::face_descriptor& face : mPolyhedronData.mFaceDescs) {
        mPolyhedronData.mIdMap[face] = i;
        ++i;
    }
    CI_LOG_I("Polyhedral mesh built, vertices: " + std::to_string(mPolyhedronData.vertices.size()) +
             ", faces: " + std::to_string(mPolyhedronData.indices.size()));
    mPolyhedronData.valid = true;
    mProgress->polyhedronPercentage = 1.0f;
}

std::array<int, 3> Geometry::gatherNeighbours(const size_t triIndex) const {
    const auto& faceDescriptors = mPolyhedronData.mFaceDescs;
    const auto& mesh = mPolyhedronData.mMesh;
    assert(triIndex < faceDescriptors.size());
    std::array<int, 3> returnValue = {-1, -1, -1};
    const PolyhedronData::face_descriptor face = faceDescriptors[triIndex];
    const auto edge = mesh.halfedge(face);
    auto itEdge = edge;

    for(int i = 0; i < 3; ++i) {
        const auto oppositeEdge = mesh.opposite(itEdge);
        if(oppositeEdge.is_valid() && !mesh.is_border(oppositeEdge)) {
            const PolyhedronData::Mesh::Face_index neighbourFace = mesh.face(oppositeEdge);
            const size_t neighbourFaceId = mPolyhedronData.mIdMap[neighbourFace];
            assert(neighbourFaceId < mTriangles.size());
            returnValue[i] = static_cast<int>(neighbourFaceId);
        }

        itEdge = mesh.next(itEdge);
    }
    assert(edge == itEdge);

    return returnValue;
}

void Geometry::computeSdf() {
    mPolyhedronData.isSdfComputed = false;
    mPolyhedronData.mMesh.remove_property_map(mPolyhedronData.sdf_property_map);
    bool created;
    boost::tie(mPolyhedronData.sdf_property_map, created) =
        mPolyhedronData.mMesh.add_property_map<PolyhedronData::face_descriptor, double>("f:sdf");
    assert(created);

    CGAL::sdf_values(mPolyhedronData.mMesh, mPolyhedronData.sdf_property_map, 2.0 / 3.0 * CGAL_PI, 25, true);
    mPolyhedronData.isSdfComputed = true;
    CI_LOG_I("SDF values computed.");
}

size_t Geometry::segment(const int numberOfClusters, const float smoothingLambda,
                         std::map<size_t, std::vector<size_t>>& segmentToTriangleIds,
                         std::unordered_map<size_t, size_t>& triangleToSegmentMap) {
    if(!mPolyhedronData.isSdfComputed) {
        return 0;
    }
    bool created;
    PolyhedronData::Mesh::Property_map<PolyhedronData::face_descriptor, std::size_t> segment_property_map;
    boost::tie(segment_property_map, created) =
        mPolyhedronData.mMesh.add_property_map<PolyhedronData::face_descriptor, std::size_t>("f:sid");
    assert(created);

    const std::size_t numberOfSegments =
        CGAL::segmentation_from_sdf_values(mPolyhedronData.mMesh, mPolyhedronData.sdf_property_map,
                                           segment_property_map, numberOfClusters, smoothingLambda);

    if(numberOfSegments > PEPR3D_MAX_PALETTE_COLORS) {
        mPolyhedronData.mMesh.remove_property_map(segment_property_map);
        return 0;
    }

    for(size_t seg = 0; seg < numberOfSegments; ++seg) {
        segmentToTriangleIds.insert({seg, {}});
    }

    // Assign the colors to the triangles
    for(const auto& face : mPolyhedronData.mFaceDescs) {
        const size_t id = mPolyhedronData.mIdMap[face];
        const size_t color = segment_property_map[face];
        assert(id < mTriangles.size());
        assert(color < numberOfSegments);
        triangleToSegmentMap.insert({id, color});
        segmentToTriangleIds[color].push_back(id);
    }

    CI_LOG_I("Segmentation finished. Number of segments: " + std::to_string(numberOfSegments));

    // End, clean up
    mPolyhedronData.mMesh.remove_property_map(segment_property_map);

    return numberOfSegments;
}

}  // namespace pepr3d
