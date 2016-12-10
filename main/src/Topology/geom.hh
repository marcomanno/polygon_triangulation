#pragma once
#include "Geo/entity.hh"
#include "Geo/vector.hh"
#include "Geo/point_in_polygon.hh"
#include "Topology/Topology.hh"

namespace Topo {

Geo::Point face_normal(Topo::Wrap<Topo::Type::FACE> _face);
Geo::Point coedge_direction(Topo::Wrap<Topo::Type::COEDGE> _coed);
namespace PointInFace {
Geo::PointInPolygon::Classification classify(
  Topo::Wrap<Topo::Type::FACE> _face, const Geo::Point& _pt);
}//namespace PointInFace

}//namespace Topo