#include "split_chain.hh"
#include "geom.hh"

#include "Geo/plane_fitting.hh"
#include "PolygonTriangularization/poly_triang.hh"
#include "Utils/error_handling.hh"
#include "Utils/circular.hh"
#include "Utils/statistics.hh"

#include <array>
#include <functional>
#include <map>
#include <set>

namespace Topo {

struct SplitChain : public ISplitChain
{
  virtual void add_chain(const VertexChain _chain)
  {
    boundaries_.push_back(_chain);
  }
  virtual void add_connection(const Topo::Wrap<Topo::Type::VERTEX>& _v0,
                              const Topo::Wrap<Topo::Type::VERTEX>& _v1,
                              bool _bidirectional = true)
  {
    connections_.emplace(Connection({ _v0, _v1 }));
    if (_bidirectional)
      connections_.emplace(Connection({ _v1, _v0 }));
  }
  void split();
  const VertexChains& boundaries() const { return boundaries_; }
  const VertexChains* boundary_islands(size_t _bondary_ind) const
  {
    auto isl_it = islands_.find(_bondary_ind);
    if (isl_it == islands_.end())
      return nullptr;
    else
      return &isl_it->second;
  }

private:
  typedef std::array<Topo::Wrap<Topo::Type::VERTEX>, 2> Connection;
  typedef std::set<Connection> Connections;

  VertexChain find_chain(Connections::iterator _conns_it);
  double find_angle(const Topo::Wrap<Topo::Type::VERTEX>& _a,
                    const Topo::Wrap<Topo::Type::VERTEX>& _b,
                    const Topo::Wrap<Topo::Type::VERTEX>& _c);

  size_t find_boundary_index(const VertexChain& _ch) const;

  void remove_chain_from_connection(
    VertexChain& _ch,
    Connections::iterator* _conn_it,
    bool _open);

  VertexChain follow_chain(const Connection& _conn,
                           std::set<Topo::Wrap<Topo::Type::VERTEX>>& _all_vert_ch);
  bool locate(const VertexChain& _ch, size_t& _loop_ind, std::array<size_t, 2>& _pos);

  VertexChains boundaries_;
  std::map<size_t, VertexChains> islands_;
  Connections connections_;
  Geo::Vector3 norm_;
};

std::shared_ptr<ISplitChain> ISplitChain::make()
{
  return std::make_shared<SplitChain>();
}

void SplitChain::split()
{
  if (boundaries_.empty())
    return;
  norm_ = Geo::vertex_polygon_normal(boundaries_[0].begin(), boundaries_[0].end());
  std::set<Topo::Wrap<Topo::Type::VERTEX>> all_chain_vertices;
  for (auto& ch : boundaries_)
    for (auto& v : ch)
      all_chain_vertices.insert(v);

  for (auto conn_it = connections_.begin(); conn_it != connections_.end(); )
  {
    if (all_chain_vertices.find((*conn_it)[0]) == all_chain_vertices.end())
      continue;
    VertexChain new_ch = follow_chain(*conn_it, all_chain_vertices);
    if (new_ch.empty())
      continue;
    std::array<size_t, 2> ins;
    size_t chain_ind;
    THROW_IF(!locate(new_ch, chain_ind, ins), "No attah chain");
    VertexChain split_chains[2];
    bool curr_chain = ins[0] > ins[1];
    for (size_t i = 0; i < boundaries_[chain_ind].size(); ++i)
    {
      if (i != ins[0] && i != ins[1])
        split_chains[curr_chain].push_back(boundaries_[chain_ind][i]);
      else
      {
        split_chains[0].push_back(boundaries_[chain_ind][i]);
        split_chains[1].push_back(boundaries_[chain_ind][i]);
        if (i == ins[0])
          split_chains[0].insert(split_chains[0].end(), new_ch.begin(), new_ch.end());
        else
          split_chains[1].insert(split_chains[0].end(), new_ch.rbegin(), new_ch.rend());
        curr_chain = !curr_chain;
      }
    }
    boundaries_[chain_ind] = std::move(split_chains[0]);
    boundaries_.push_back(std::move(split_chains[1]));
    remove_chain_from_connection(new_ch, nullptr, true);
    all_chain_vertices.insert(new_ch.begin(), new_ch.end());
  }

  VertexChains islands;
  for (auto conn_it = connections_.begin(); 
       conn_it != connections_.end(); )
  {
    auto new_ch = find_chain(conn_it);
    ++conn_it;
    if (new_ch.empty())
      continue;
    boundaries_.push_back(new_ch);
    std::reverse(new_ch.begin(), new_ch.end());
    islands.push_back(std::move(new_ch));
    remove_chain_from_connection(new_ch, &conn_it, false);
  }
  for (auto& ch : islands)
  {
    auto ind = find_boundary_index(ch);
    islands_[ind].push_back(std::move(ch));
  }
}

VertexChain SplitChain::find_chain(Connections::iterator _conns_it)
{
  std::vector<Connections::iterator> edge_ch;
  std::vector<std::tuple<Connections::iterator, size_t>> branches;
  branches.emplace_back(_conns_it, 0);
  VertexChain result;
  while (!branches.empty())
  {
    edge_ch.resize(std::get<size_t>(branches.back()) + 1);
    edge_ch.back() = std::get<Connections::iterator>(branches.back());
    branches.pop_back();
    const auto& edge = *edge_ch.back();
    if (edge[1] == (*(edge_ch[0]))[0])
    {
      for (auto& ed : edge_ch)
        result.push_back((*ed)[0]);
      auto norm = Geo::vertex_polygon_normal(result.begin(), result.end());
      if (norm_ * norm > 0)
        break;
      result.clear();
    }

    auto& end_v = edge[1];
    auto pos =
      connections_.lower_bound(Connection{ end_v, Topo::Wrap<Topo::Type::VERTEX>() });
    double min_ang = std::numeric_limits<double>::max();
    std::vector<std::tuple<double, Connections::iterator>> choices;
    const double ANG_EPS = 1e-8;
    for (; pos != connections_.end() && (*pos)[0] == end_v; )
    {
      auto ang = find_angle(edge[0], edge[1], (*pos)[1]);
      Utils::a_eq_b_if_a_gt_b(min_ang, ang);
      if (ang > 2 * M_PI - ANG_EPS && (*pos)[1] != edge[0])
        ang = 0;
      choices.emplace_back(ang, pos);
    }
    for (const auto& achoice : choices)
    {
      if (std::get<double>(achoice) > min_ang + ANG_EPS)
        continue;
      branches.emplace_back(std::get<Connections::iterator>(achoice), branches.size());
    }
  }
  return result;
}

double SplitChain::find_angle(
  const Topo::Wrap<Topo::Type::VERTEX>& _a, 
  const Topo::Wrap<Topo::Type::VERTEX>& _b,
  const Topo::Wrap<Topo::Type::VERTEX>& _c)
{
  Geo::Point pts[3];
  _a->geom(pts[0]);
  _b->geom(pts[1]);
  _c->geom(pts[2]);
  auto v0 = pts[0] - pts[1];
  auto v1 = pts[2] - pts[1];
  auto ang = Geo::signed_angle(v0, v1, norm_);
  if (ang < 0)
    ang = 2 * M_PI + ang;
  return ang;
}

VertexChain SplitChain::follow_chain(
  const Connection& _conn,
  std::set<Topo::Wrap<Topo::Type::VERTEX>>& _all_vert_ch)
{
  Connection curr_conn = _conn;
  VertexChain v_ch;
  v_ch.push_back(curr_conn[0]);
  v_ch.push_back(curr_conn[1]);
  while (_all_vert_ch.find(v_ch.back()) == _all_vert_ch.end())
  {
    auto pos =
      connections_.lower_bound(Connection{ curr_conn[1], Topo::Wrap<Topo::Type::VERTEX>() });
    if (pos == connections_.end())
      return VertexChain();
    curr_conn = *pos;
    v_ch.push_back(curr_conn[1]);
  }
  return v_ch;
}

bool SplitChain::locate(
  const VertexChain& _ch, size_t& _loop_ind,
  std::array<size_t, 2>& _pos)
{
  typedef std::array<size_t, 2> InsPoint;
  std::vector<InsPoint> choices[2];
  for (auto i = boundaries_.size(); i-- > 0;)
  {
    for (auto j = boundaries_[i].size(); j-- > 0;)
    {
      size_t ins;
      if (boundaries_[i][j] == _ch.front())     ins = 0;
      else if (boundaries_[i][j] == _ch.back()) ins = 1;
      else
        continue;
      choices[ins].push_back({ i, j });
    }
  }
  // Start and end points must be inside the same chain.
  // Remove ins points that are using chains not present in
  // the other point.
  for (size_t i = 0; i < std::size(choices); ++i)
  {
    if (choices[i].size() < 2)
      continue;
    for (auto j = choices[i].size(); j-- > 0;)
    {
      auto it_ch =
        std::lower_bound(choices[1 - i].begin(), choices[1 - i].end(),
                         InsPoint({ choices[i][j][0], 0 }),
                         std::greater<InsPoint>());
      if (it_ch == choices[1 - i].end() || choices[i][j][0] != (*it_ch)[0])
        choices[i].erase(choices[i].begin() + j);
    }
  }
  // Select the chain
  if (choices[0].front()[0] != choices[0].back()[0])
  {
    Geo::Point pt_in;
    _ch[1]->geom(pt_in);
    if (_ch.size() == 2)
    {
      Geo::Point pt_0;
      _ch[0]->geom(pt_0);
      pt_in += pt_0;
      pt_in /= 2.;
    }
    // There is more than one chain.
    size_t prev_chain_ind = std::numeric_limits<size_t>::max();
    size_t sel_chain_ind = std::numeric_limits<size_t>::max();
    for (auto& ins_pt : choices[0])
    {
      if (prev_chain_ind == ins_pt[0])
        continue;

      prev_chain_ind = ins_pt[0];
      auto pt_cl = PointInFace::classify(boundaries_[ins_pt[0]], pt_in);
      if (pt_cl == Geo::PointInPolygon::Classification::Inside)
      {
        sel_chain_ind = ins_pt[0];
        break;
      }
    }
    THROW_IF(sel_chain_ind == std::numeric_limits<size_t>::max(),
             "no loop contains the chain");
    for (auto& ch : choices)
      for (auto i = ch.size(); i-- > 0;)
        if (ch[i][0] != sel_chain_ind)
          ch.erase(ch.begin() + i);
  }
  // Select the insetion point.
  Topo::Wrap<Topo::Type::VERTEX> vv[2] = { _ch[1], _ch[_ch.size() - 1] };
  for (auto i : { 0, 1 })
  {
    auto& cur = choices[i];
    if (cur.size() < 2)
      continue;
    for (auto j = cur.size(); j-- > 0; )
    {
      auto& achoice = cur[j];
      auto& sel_ch = boundaries_[achoice[0]];
      Topo::Wrap<Topo::Type::VERTEX>* vert_ptrs[4] = {
        &sel_ch[Utils::decrease(achoice[1], sel_ch.size())],
        &sel_ch[achoice[1]],
        &sel_ch[Utils::increase(achoice[1], sel_ch.size())],
        &vv[i]
      };
      auto out_angle = find_angle(*vert_ptrs[0], *vert_ptrs[1], *vert_ptrs[2]);
      auto int_angle = find_angle(*vert_ptrs[0], *vert_ptrs[1], vv[i]);
      if (int_angle > out_angle)
        cur.erase(cur.begin() + j);
    }
  }
  if (choices[0].size() != 1 || choices[1].size() != 1)
    return false;
  _loop_ind = choices[0].front()[0];
  _pos[0] = choices[0].front()[1];
  _pos[1] = choices[1].front()[1];
  return true;
}

void SplitChain::remove_chain_from_connection(
  VertexChain& _ch,
  Connections::iterator* _conn_it,
  bool _open)
{
  auto prev_vert_it = _open ? _ch.begin() : std::prev(_ch.end());
  for (auto vert_it = _ch.begin() + _open;  vert_it != _ch.begin();
       prev_vert_it = vert_it++)
  {
    auto remove_connection = [this, _conn_it](
      const Wrap<Type::VERTEX>& _a,
      const Wrap<Type::VERTEX>& _b)
    {
      auto used_conn_it = connections_.find(Connection{ _a , _b });
      if (used_conn_it != connections_.end())
      {
        if (_conn_it != nullptr && used_conn_it == *_conn_it)
          ++(*_conn_it);
        connections_.erase(used_conn_it);
      }
    };
    remove_connection(*prev_vert_it, *vert_it);
    remove_connection(*vert_it, *prev_vert_it);
  }
}

size_t SplitChain::find_boundary_index(const VertexChain& _ch) const
{
  THROW_IF(_ch.empty(), "Empty chain ov vertices");
  Geo::Point pt;
  _ch[0]->geom(pt);
  std::vector<size_t> choices;
  for (auto i = boundaries_.size(); i-- > 0; )
  {
    if (PointInFace::classify(boundaries_[i], pt) ==
        Geo::PointInPolygon::Classification::Inside)
    {
      choices.push_back(i);
    }
  }
  THROW_IF(choices.empty(), "Island without a boundary");
  if (choices.size() == 1)
    return choices[0];
  double min_area = std::numeric_limits<double>::max();
  size_t min_ind = 0;
  for (auto i : choices)
  {
    std::vector<Geo::Point> pts;
    pts.reserve(boundaries_[i].size());
    for (auto& v : boundaries_[i])
    {
      pts.emplace_back();
      v->geom(pts.back());
    }
    auto poly_t = Geo::IPolygonTriangulation::make();
    poly_t->add(pts);
    auto area = poly_t->area();
    if (Utils::a_eq_b_if_a_lt_b(min_area, area))
      min_ind = i;
  }
  return min_ind;
}

} // namespace Topo