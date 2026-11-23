#pragma once
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>

// Apple6: capture each input vertex once, then execute the same geometry program
// in an ordinary vertex function. Fixed output slots outside the emitted range
// are clipped. No CPU readback, guest-memory rewrite, or mesh API is needed.
namespace MetalGeometryFallback {
constexpr uint32_t PayloadBinding = 28, DrawBinding = 29;
struct Draw { int32_t baseVertex; uint32_t baseInstance, verticesPerInstance, primitivesPerInstance; };
constexpr uint32_t ExpandedVertices(uint32_t topology, uint32_t vertices = 32) {
    return topology == 0 ? vertices : topology == 1 ? (vertices > 1 ? (vertices - 1) * 2 : 0) : (vertices > 2 ? (vertices - 2) * 3 : 0);
}
inline void Replace(std::string& source, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = source.find(from, at)) != std::string::npos) { source.replace(at, from.size(), to); at += to.size(); }
}
inline std::string CapturePrelude(bool connected) {
    return "uint tig = labVertex / VERTICES_PER_VERTEX_PRIMITIVE; uint tid = labVertex % VERTICES_PER_VERTEX_PRIMITIVE;\n"
        "device ObjectPayload& objectPayload = labPayloads[tig]; uint primitive = tig % labDraw.primitivesPerInstance;\n" +
        std::string(connected ? "uint vid = primitive + tid;\n" : "uint vid = primitive * VERTICES_PER_VERTEX_PRIMITIVE + tid;\n") +
        "uint iid = tig / labDraw.primitivesPerInstance + labDraw.baseInstance;\n"
        "VertexIn in = fetchVertex(vid, iid, indexBuffer, indexType, labDraw.baseVertex VERTEX_BUFFERS);\n"
        "device VertexOut& out = objectPayload.vertexOut[tid];\n";
}
inline std::string GeometrySource(std::string source, uint32_t topology, bool rectangle = false) {
    if (topology > 2) throw std::runtime_error("Unsupported geometry output topology");
    const auto alias = source.find("using MeshType = mesh<");
    const auto endAlias = source.find(';', alias);
    const auto entry = source.find("[[mesh,");
    const auto main = source.find("void main0(", entry);
    if (alias == std::string::npos || endAlias == std::string::npos || entry == std::string::npos || main == std::string::npos)
        throw std::runtime_error("Unrecognized Metal geometry program");
    source.replace(entry, main + 5 - entry, "vertex GeometryOut ");
    source.replace(alias, endAlias + 1 - alias, R"MSL(
struct MeshType {
    GeometryOut vertices[32]; uint indices[96]; uint count;
    void set_vertex(uint i, GeometryOut value) thread { if (i < 32) vertices[i] = value; }
    void set_index(uint i, uint value) thread { if (i < 96) indices[i] = value; }
    void set_primitive_count(uint value) thread { count = value; }
};
)MSL");
    const std::string old = "MeshType mesh, const object_data ObjectPayload& objectPayload [[payload]]";
    const auto args = source.find(old);
    if (args == std::string::npos) throw std::runtime_error("Unrecognized Metal geometry inputs");
    const uint32_t slots = rectangle ? 6 : ExpandedVertices(topology);
    source.replace(args, old.size(), "uint labVertex [[vertex_id]], const device ObjectPayload* labPayloads [[buffer(28)]]");
    const auto body = source.find('{', source.find("main0("));
    source.insert(body + 1, "\nMeshType mesh{};\nconst device ObjectPayload& objectPayload = labPayloads[labVertex / " + std::to_string(slots) +
        "];\nuint labLane = labVertex % " + std::to_string(slots) + ";\nuint gl_PrimitiveIDIn = labVertex / " + std::to_string(slots) + ";\n");
    // Geometry transform feedback executes once per source primitive, not once
    // per expanded raster vertex. The remaining invocations only read inputs.
    source = std::regex_replace(source, std::regex(R"((\bsb\[[^\r\n;]+;))"), "if (labLane == 0) $1");
    const auto tail = source.rfind('}');
    source.insert(tail, "\nif (labLane < min(mesh.count, " + std::to_string(rectangle ? 2 : topology == 0 ? 32 : topology == 1 ? 31 : 30) +
        "u) * " + std::to_string(topology + 1) + "u && mesh.indices[labLane] < 32) return mesh.vertices[mesh.indices[labLane]];\n"
        "GeometryOut clipped{}; clipped.position = float4(2.0, 2.0, 0.0, 1.0); return clipped;\n");
    return source;
}
}
