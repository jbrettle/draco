// Copyright 2016 The Draco Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "mesh/corner_table.h"
#include "mesh/corner_table_iterators.h"

namespace draco {

CornerTable::CornerTable()
    : num_original_vertices_(0),
      num_degenerated_faces_(0),
      num_isolated_vertices_(0) {}

std::unique_ptr<CornerTable> CornerTable::Create(
    const IndexTypeVector<FaceIndex, FaceType> &faces) {
  std::unique_ptr<CornerTable> ct(new CornerTable());
  if (!ct->Initialize(faces))
    return nullptr;
  return ct;
}

bool CornerTable::Initialize(
    const IndexTypeVector<FaceIndex, FaceType> &faces) {
  faces_ = faces;
  int num_vertices = -1;
  if (!ComputeOppositeCorners(&num_vertices))
    return false;
  if (!ComputeVertexCorners(num_vertices))
    return false;
  return true;
}

void CornerTable::Reset(int num_faces) {
  faces_.assign(num_faces, kInvalidFace);
  opposite_corners_.assign(num_faces * 3, kInvalidCornerIndex);
  vertex_corners_.reserve(num_faces * 3);
}

bool CornerTable::ComputeOppositeCorners(int *num_vertices) {
  if (num_vertices == nullptr)
    return false;
  opposite_corners_.resize(num_corners(), kInvalidCornerIndex);

  // Out implementation for finding opposite corners is based on keeping track
  // of outgoing half-edges for each vertex of the mesh. Half-edges (defined by
  // their opposite corners) are processed one by one and whenever a new
  // half-edge (corner) is processed, we check whether the sink vertex of
  // this half-edge contains its sibling half-edge. If yes, we connect them and
  // remove the sibling half-edge from the sink vertex, otherwise we add the new
  // half-edge to its source vertex.

  // First compute the number of outgoing half-edges (corners) attached to each
  // vertex.
  VertexIndex max_vertex_index(-1);
  std::vector<int> num_corners_on_vertices;
  num_corners_on_vertices.reserve(num_corners());
  for (CornerIndex c(0); c < num_corners(); ++c) {
    const VertexIndex v1 = Vertex(c);
    if (v1.value() >= num_corners_on_vertices.size())
      num_corners_on_vertices.resize(v1.value() + 1, 0);
    // For each corner there is always exactly one outgoing half-edge attached
    // to its vertex.
    num_corners_on_vertices[v1.value()]++;
  }

  // Create a storage for half-edges on each vertex. We store all half-edges in
  // one array, where each entry is identified by the half-edge's sink vertex id
  // and the associated half-edge corner id (corner opposite to the half-edge).
  // Each vertex will be assigned storage for up to
  // |num_corners_on_vertices[vert_id]| half-edges. Unused half-edges are marked
  // with |sink_vert| == -1.
  struct VertexEdgePair {
    VertexEdgePair() : sink_vert(-1), edge_corner(-1) {}
    VertexIndex sink_vert;
    CornerIndex edge_corner;
  };
  std::vector<VertexEdgePair> vertex_edges(num_corners(), VertexEdgePair());

  // For each vertex compute the offset (location where the first half-edge
  // entry of a given vertex is going to be stored). This way each vertex is
  // guaranteed to have a non-overlapping storage with respect to the other
  // vertices.
  std::vector<int> vertex_offset(num_corners_on_vertices.size());
  int offset = 0;
  for (int i = 0; i < num_corners_on_vertices.size(); ++i) {
    vertex_offset[i] = offset;
    offset += num_corners_on_vertices[i];
  }

  // Now go over the all half-edges (using their opposite corners) and either
  // insert them to the |vertex_edge| array or connect them with existing
  // half-edges.
  for (CornerIndex c(0); c < num_corners(); ++c) {
    const VertexIndex source_v = Vertex(Next(c));
    const VertexIndex sink_v = Vertex(Previous(c));

    const FaceIndex face_index = Face(c);
    if (c == FirstCorner(face_index)) {
      // Check whether the face is degenerated, if so ignore it.
      const VertexIndex v0 = Vertex(c);
      if (v0 == source_v || v0 == sink_v || source_v == sink_v) {
        ++num_degenerated_faces_;
        c += 2;  // Ignore the next two corners of the same face.
        continue;
      }
    }

    CornerIndex opposite_c(-1);
    // The maximum number of half-edges attached to the sink vertex.
    const int num_corners_on_vert = num_corners_on_vertices[sink_v.value()];
    // Where to look for the first half-edge on the sink vertex.
    int offset = vertex_offset[sink_v.value()];
    for (int i = 0; i < num_corners_on_vert; ++i, ++offset) {
      const VertexIndex other_v = vertex_edges[offset].sink_vert;
      if (other_v < 0)
        break;  // No matching half-edge found on the sink vertex.
      if (other_v == source_v) {
        // A matching half-edge was found on the sink vertex. Mark the
        // half-edge's opposite corner.
        opposite_c = vertex_edges[offset].edge_corner;
        // Remove the half-edge from the sink vertex. We remap all subsequent
        // half-edges one slot down.
        // TODO(ostava): This can be optimized a little bit, by remaping only
        // the half-edge on the last valid slot into the deleted half-edge's
        // slot.
        for (int j = i + 1; j < num_corners_on_vert; ++j, ++offset) {
          vertex_edges[offset] = vertex_edges[offset + 1];
          if (vertex_edges[offset].sink_vert < 0)
            break;  // Unused half-edge reached.
        }
        // Mark the last entry as unused.
        vertex_edges[offset].sink_vert = VertexIndex(-1);
        break;
      }
    }
    if (opposite_c < 0) {
      // No opposite corner found. Insert the new edge
      const int num_corners_on_source_vert =
          num_corners_on_vertices[source_v.value()];
      int offset = vertex_offset[source_v.value()];
      for (int i = 0; i < num_corners_on_source_vert; ++i, ++offset) {
        // Find the first unused half-edge slot on the source vertex.
        if (vertex_edges[offset].sink_vert < 0) {
          vertex_edges[offset].sink_vert = sink_v;
          vertex_edges[offset].edge_corner = c;
          break;
        }
      }
    } else {
      // Opposite corner found.
      opposite_corners_[c] = opposite_c;
      opposite_corners_[opposite_c] = c;
    }
  }
  *num_vertices = num_corners_on_vertices.size();
  return true;
}

bool CornerTable::ComputeVertexCorners(int num_vertices) {
  num_original_vertices_ = num_vertices;
  vertex_corners_.resize(num_vertices, kInvalidCornerIndex);
  // Arrays for marking visited vertices and corners that allow us to detect
  // non-manifold vertices.
  std::vector<bool> visited_vertices(num_vertices, false);
  std::vector<bool> visited_corners(num_corners(), false);

  for (FaceIndex f(0); f < faces_.size(); ++f) {
    const CornerIndex first_face_corner = FirstCorner(f);
    // Check whether the face is degenerated. If so ignore it.
    if (IsDegenerated(f))
      continue;

    for (int k = 0; k < 3; ++k) {
      const CornerIndex c = first_face_corner + k;
      if (visited_corners[c.value()])
        continue;
      VertexIndex v(faces_[f][k]);
      // Note that one vertex maps to many corners, but we just keep track
      // of the vertex which has a boundary on the left if the vertex lies on
      // the boundary. This means that all the related corners can be accessed
      // by iterating over the SwingRight() operator.
      // In case of a vertex inside the mesh, the choice is arbitrary.
      bool is_non_manifold_vertex = false;
      if (visited_vertices[v.value()]) {
        // A visited vertex of an unvisited corner found. Must be a non-manifold
        // vertex.
        // Create a new vertex for it.
        vertex_corners_.push_back(kInvalidCornerIndex);
        non_manifold_vertex_parents_.push_back(v);
        visited_vertices.push_back(false);
        v = VertexIndex(num_vertices++);
        is_non_manifold_vertex = true;
      }
      // Mark the vertex as visited.
      visited_vertices[v.value()] = true;

      // First swing all the way to the left and mark all corners on the way.
      CornerIndex act_c(c);
      while (act_c != kInvalidCornerIndex) {
        visited_corners[act_c.value()] = true;
        // Vertex will eventually point to the left most corner.
        vertex_corners_[v] = act_c;
        if (is_non_manifold_vertex) {
          // Update vertex index in the corresponding face.
          FaceIndex act_f = Face(act_c);
          faces_[act_f][LocalIndex(act_c)] = v;
        }
        act_c = SwingLeft(act_c);
        if (act_c == c)
          break;  // Full circle reached.
      }
      if (act_c == kInvalidCornerIndex) {
        // If we have reached an open boundary we need to swing right from the
        // initial corner to mark all corners in the opposite direction.
        act_c = SwingRight(c);
        while (act_c != kInvalidCornerIndex) {
          visited_corners[act_c.value()] = true;
          if (is_non_manifold_vertex) {
            // Update vertex index in the corresponding face.
            FaceIndex act_f = Face(act_c);
            faces_[act_f][LocalIndex(act_c)] = v;
          }
          act_c = SwingRight(act_c);
        }
      }
    }
  }

  // Count the number of isolated (unprocessed) vertices.
  num_isolated_vertices_ = 0;
  for (bool visited : visited_vertices) {
    if (!visited)
      ++num_isolated_vertices_;
  }
  return true;
}

bool CornerTable::IsDegenerated(FaceIndex face) const {
  if (face == kInvalidFaceIndex)
    return true;
  const CornerIndex first_face_corner = FirstCorner(face);
  const VertexIndex v0 = Vertex(first_face_corner);
  const VertexIndex v1 = Vertex(Next(first_face_corner));
  const VertexIndex v2 = Vertex(Previous(first_face_corner));
  if (v0 == v1 || v0 == v2 || v1 == v2)
    return true;
  return false;
}

int CornerTable::Valence(VertexIndex v) const {
  if (v == kInvalidVertexIndex)
    return -1;
  VertexRingIterator vi(this, v);
  int valence = 0;
  for (; !vi.End(); vi.Next()) {
    ++valence;
  }
  return valence;
}

void CornerTable::UpdateFaceToVertexMap(const VertexIndex vertex) {
  VertexCornersIterator<CornerTable> it(this, vertex);
  for (; !it.End(); ++it) {
    const CornerIndex corner = *it;
    const FaceIndex face = Face(corner);
    faces_[face][LocalIndex(corner)] = vertex;
  }
}

}  // namespace draco
