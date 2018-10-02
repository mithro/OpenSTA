// OpenSTA, Static Timing Analyzer
// Copyright (c) 2018, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <algorithm>
#include <cmath> // abs
#include "Machine.hh"
#include "DisallowCopyAssign.hh"
#include "ThreadForEach.hh"
#include "Mutex.hh"
#include "Report.hh"
#include "Debug.hh"
#include "Error.hh"
#include "Stats.hh"
#include "Fuzzy.hh"
#include "TimingRole.hh"
#include "FuncExpr.hh"
#include "TimingArc.hh"
#include "Sequential.hh"
#include "Units.hh"
#include "PortDirection.hh"
#include "Liberty.hh"
#include "Network.hh"
#include "Graph.hh"
#include "GraphCmp.hh"
#include "Levelize.hh"
#include "PortDelay.hh"
#include "Clock.hh"
#include "CycleAccting.hh"
#include "ExceptionPath.hh"
#include "DataCheck.hh"
#include "Sdc.hh"
#include "SearchPred.hh"
#include "Bfs.hh"
#include "DcalcAnalysisPt.hh"
#include "Corner.hh"
#include "Sim.hh"
#include "PathVertex.hh"
#include "PathVertexRep.hh"
#include "PathRef.hh"
#include "ClkInfo.hh"
#include "Tag.hh"
#include "TagGroup.hh"
#include "PathEnd.hh"
#include "PathGroup.hh"
#include "PathAnalysisPt.hh"
#include "VisitPathEnds.hh"
#include "GatedClk.hh"
#include "WorstSlack.hh"
#include "Latches.hh"
#include "Crpr.hh"
#include "Genclks.hh"
#include "Search.hh"

namespace sta {

using std::min;
using std::max;
using std::abs;

////////////////////////////////////////////////////////////////

EvalPred::EvalPred(const StaState *sta) :
  SearchPred0(sta),
  search_thru_latches_(true)
{
}

void
EvalPred::setSearchThruLatches(bool thru_latches)
{
  search_thru_latches_ = thru_latches;
}

bool
EvalPred::searchThru(Edge *edge)
{
  const Sdc *sdc = sta_->sdc();
  TimingRole *role = edge->role();
  return SearchPred0::searchThru(edge)
    && (sdc->dynamicLoopBreaking()
	|| !edge->isDisabledLoop())
    && !role->isTimingCheck()
    && (search_thru_latches_
	|| role != TimingRole::latchDtoQ()
	|| sta_->latches()->latchDtoQState(edge) == latch_state_open);
}

bool
EvalPred::searchTo(const Vertex *to_vertex)
{
  const Sdc *sdc = sta_->sdc();
  const Pin *pin = to_vertex->pin();
  return SearchPred0::searchTo(to_vertex)
    && !(sdc->isVertexPinClock(pin)
	 && !sdc->isPathDelayInternalEndpoint(pin));
}

////////////////////////////////////////////////////////////////

DynLoopSrchPred::DynLoopSrchPred(TagGroupBldr *tag_bldr) :
  tag_bldr_(tag_bldr)
{
}

bool
DynLoopSrchPred::loopEnabled(Edge *edge,
			     const Sdc *sdc,
			     const Graph *graph,
			     Search *search)
{
  return !edge->isDisabledLoop()
    || (sdc->dynamicLoopBreaking()
	&& hasPendingLoopPaths(edge, graph, search));
}

bool
DynLoopSrchPred::hasPendingLoopPaths(Edge *edge,
				     const Graph *graph,
				     Search *search)
{
  if (tag_bldr_
      && tag_bldr_->hasLoopTag()) {
    Corners *corners = search->corners();
    Vertex *from_vertex = edge->from(graph);
    TagGroup *prev_tag_group = search->tagGroup(from_vertex);
    ArrivalMap::Iterator arrival_iter(tag_bldr_->arrivalMap());
    while (arrival_iter.hasNext()) {
      Tag *from_tag;
      int arrival_index;
      arrival_iter.next(from_tag, arrival_index);
      if (from_tag->isLoop()) {
	// Loop false path exceptions apply to rise/fall edges so to_tr
	// does not matter.
	PathAPIndex path_ap_index = from_tag->pathAPIndex();
	PathAnalysisPt *path_ap = corners->findPathAnalysisPt(path_ap_index);
	Tag *to_tag = search->thruTag(from_tag, edge, TransRiseFall::rise(),
				      path_ap->pathMinMax(), path_ap);
	if (to_tag
	    && (prev_tag_group == NULL
		|| !prev_tag_group->hasTag(from_tag)))
	  return true;
      }
    }
  }
  return false;
}

// EvalPred unless
//  latch D->Q edge
class SearchThru : public EvalPred, public DynLoopSrchPred
{
public:
  SearchThru(TagGroupBldr *tag_bldr,
	     const StaState *sta);
  virtual bool searchThru(Edge *edge);

private:
  DISALLOW_COPY_AND_ASSIGN(SearchThru);
};

SearchThru::SearchThru(TagGroupBldr *tag_bldr,
		       const StaState *sta) :
  EvalPred(sta),
  DynLoopSrchPred(tag_bldr)
{
}

bool
SearchThru::searchThru(Edge *edge)
{
  const Graph *graph = sta_->graph();
  const Sdc *sdc = sta_->sdc();
  Search *search = sta_->search();
  return EvalPred::searchThru(edge)
    // Only search thru latch D->Q if it is always open.
    // Enqueue thru latches is handled explicitly by search.
    && (edge->role() != TimingRole::latchDtoQ()
	|| sta_->latches()->latchDtoQState(edge) == latch_state_open)
    && loopEnabled(edge, sdc, graph, search);
}

class ClkArrivalSearchPred : public EvalPred
{
public:
  ClkArrivalSearchPred(const StaState *sta);
  virtual bool searchThru(Edge *edge);

private:
  DISALLOW_COPY_AND_ASSIGN(ClkArrivalSearchPred);
};

ClkArrivalSearchPred::ClkArrivalSearchPred(const StaState *sta) :
  EvalPred(sta)
{
}

bool
ClkArrivalSearchPred::searchThru(Edge *edge)
{
  const TimingRole *role = edge->role();
  return (role->isWire()
	  || role == TimingRole::combinational())
    && EvalPred::searchThru(edge);
}

////////////////////////////////////////////////////////////////

Search::Search(StaState *sta) :
  StaState(sta)
{
  init(sta);
}

void
Search::init(StaState *sta)
{
  report_unconstrained_paths_ = false;
  search_adj_ = new SearchThru(NULL, sta);
  eval_pred_ = new EvalPred(sta);
  crpr_ = new Crpr(sta);
  genclks_ = new Genclks(sta);
  arrival_visitor_ = new ArrivalVisitor(sta);
  clk_arrivals_valid_ = false;
  arrivals_exist_ = false;
  arrivals_at_endpoints_exist_ = false;
  arrivals_seeded_ = false;
  requireds_exist_ = false;
  requireds_seeded_ = false;
  tns_exists_ = false;
  worst_slacks_ = NULL;
  arrival_iter_ = new BfsFwdIterator(bfs_arrival, NULL, sta);
  required_iter_ = new BfsBkwdIterator(bfs_required, search_adj_, sta);
  tag_capacity_ = 127;
  tag_set_ = new TagHashSet(tag_capacity_, false);
  clk_info_set_ = new ClkInfoSet(ClkInfoLess(sta));
  tag_count_ = 0;
  tags_ = new Tag*[tag_capacity_];
  tag_group_capacity_ = 127;
  tag_groups_ = new TagGroup*[tag_group_capacity_];
  tag_group_count_ = 0;
  tag_group_set_ = new TagGroupSet(tag_group_capacity_, false);
  visit_path_ends_ = new VisitPathEnds(this);
  gated_clk_ = new GatedClk(this);
  path_groups_ = NULL;
  endpoints_ = NULL;
  invalid_endpoints_ = NULL;
  filter_ = NULL;
  filter_from_ = NULL;
  filter_to_ = NULL;
  have_paths_ = false;
  found_downstream_clk_pins_ = false;
}

Search::~Search()
{
  deletePaths();
  deleteTags();
  delete tag_set_;
  delete clk_info_set_;
  delete [] tags_;
  delete [] tag_groups_;
  delete tag_group_set_;
  delete search_adj_;
  delete eval_pred_;
  delete arrival_visitor_;
  delete arrival_iter_;
  delete required_iter_;
  delete endpoints_;
  delete invalid_endpoints_;
  delete visit_path_ends_;
  delete gated_clk_;
  delete worst_slacks_;
  delete crpr_;
  delete genclks_;
  deleteFilter();
  deletePathGroups();
}

void
Search::clear()
{
  clk_arrivals_valid_ = false;
  arrivals_exist_ = false;
  arrivals_at_endpoints_exist_ = false;
  arrivals_seeded_ = false;
  requireds_exist_ = false;
  requireds_seeded_ = false;
  tns_exists_ = false;
  clearWorstSlack();
  invalid_arrivals_.clear();
  arrival_iter_->clear();
  invalid_requireds_.clear();
  invalid_tns_.clear();
  required_iter_->clear();
  endpointsInvalid();
  deletePathGroups();
  deletePaths();
  deleteTags();
  clearPendingLatchOutputs();
  deleteFilter();
  genclks_->clear();
  found_downstream_clk_pins_ = false;
}

void
Search::setReportUnconstrainedPaths(bool report)
{
  if (report_unconstrained_paths_ != report)
    arrivalsInvalid();
  report_unconstrained_paths_ = report;
}

void
Search::deleteTags()
{
  for (TagGroupIndex i = 0; i < tag_group_count_; i++) {
    TagGroup *group = tag_groups_[i];
    delete group;
  }
  tag_group_count_ = 0;
  tag_group_set_->clear();

  tag_count_ = 0;
  tag_set_->deleteContentsClear();

  clk_info_set_->deleteContentsClear();
}

void
Search::deleteFilter()
{
  if (filter_) {
    sdc_->deleteException(filter_);
    filter_ = NULL;
    filter_from_ = NULL;
  }
  else {
    // Filter owns filter_from_ if it exists.
    delete filter_from_;
    filter_from_ = NULL;
  }
  delete filter_to_;
  filter_to_ = NULL;
}

void
Search::copyState(const StaState *sta)
{
  StaState::copyState(sta);
  // Notify sub-components.
  arrival_iter_->copyState(sta);
  required_iter_->copyState(sta);
  visit_path_ends_->copyState(sta);
  gated_clk_->copyState(sta);
  crpr_->copyState(sta);
  genclks_->copyState(sta);
}

////////////////////////////////////////////////////////////////

void
Search::deletePaths()
{
  debugPrint0(debug_, "search", 1, "delete paths\n");
  if (have_paths_) {
    VertexIterator vertex_iter(graph_);
    while (vertex_iter.hasNext()) {
      Vertex *vertex = vertex_iter.next();
      deletePaths1(vertex);
    }
    have_paths_ = false;
  }
}

void
Search::deletePaths1(Vertex *vertex)
{
  Arrival *arrivals = vertex->arrivals();
  delete [] arrivals;
  vertex->setArrivals(NULL);
  PathVertexRep *prev_paths = vertex->prevPaths();
  delete [] prev_paths;
  vertex->setPrevPaths(NULL);
  vertex->setTagGroupIndex(tag_group_index_max);
  vertex->setHasRequireds(false);
}

void
Search::deletePaths(Vertex *vertex)
{
  tnsNotifyBefore(vertex);
  if (worst_slacks_)
    worst_slacks_->worstSlackNotifyBefore(vertex);
  deletePaths1(vertex);
}

////////////////////////////////////////////////////////////////

// from/thrus/to are owned and deleted by Search.
// Returned sequence is owned by the caller.
// PathEnds are owned by Search PathGroups and deleted on next call.
PathEndSeq *
Search::findPathEnds(ExceptionFrom *from,
		     ExceptionThruSeq *thrus,
		     ExceptionTo *to,
		     const Corner *corner,
		     const MinMaxAll *min_max,
		     int max_paths,
		     int nworst,
		     bool unique_pins,
		     float slack_min,
		     float slack_max,
		     bool sort_by_slack,
		     PathGroupNameSet *group_names,
		     bool setup,
		     bool hold,
		     bool recovery,
		     bool removal,
		     bool clk_gating_setup,
		     bool clk_gating_hold)
{
  // Delete results from last findPathEnds.
  // Filtered arrivals are deleted by Sta::searchPreamble.
  deletePathGroups();
  checkFromThrusTo(from, thrus, to);
  filter_from_ = from;
  filter_to_ = to;
  if ((from
       && (from->pins()
	   || from->instances()))
      || thrus) {
    filter_ = sdc_->makeFilterPath(from, thrus, NULL);
    findFilteredArrivals();
  }
  else
    // These cases do not require filtered arrivals.
    //  -from clocks
    //  -to
    findAllArrivals();
  if (!sdc_->recoveryRemovalChecksEnabled())
    recovery = removal = false;
  if (!sdc_->gatedClkChecksEnabled())
    clk_gating_setup = clk_gating_hold = false;
  path_groups_ = makePathGroups(max_paths, nworst, unique_pins,
				slack_min, slack_max,
				group_names, setup, hold,
				recovery, removal,
				clk_gating_setup, clk_gating_hold);
  ensureDownstreamClkPins();
  PathEndSeq *path_ends = path_groups_->makePathEnds(to, corner, min_max,
						     sort_by_slack);
  sdc_->reportClkToClkMaxCycleWarnings();
  return path_ends;
}

// From/thrus/to are used to make a filter exception.  If the last
// search used a filter arrival/required times were only found for a
// subset of the paths.  Delete the paths that have a filter
// exception state.
void
Search::deleteFilteredArrivals()
{
  if (filter_) {
    ExceptionFrom *from = filter_->from();
    ExceptionThruSeq *thrus = filter_->thrus();
    if ((from
	 && (from->pins()
	     || from->instances()))
	|| thrus) {
      VertexIterator vertex_iter(graph_);
      while (vertex_iter.hasNext()) {
	Vertex *vertex = vertex_iter.next();
	TagGroup *tag_group = tagGroup(vertex);
	if (tag_group
	    && tag_group->hasFilterTag()) {
	  // Vertex's tag_group will be deleted.
	  deletePaths(vertex);
	  arrivalInvalid(vertex);
	  requiredInvalid(vertex);
	}
      }
      deleteFilterTagGroups();
      deleteFilterClkInfos();
      deleteFilterTags();
    }
  }
  deleteFilter();
}

void
Search::deleteFilterTagGroups()
{
  for (TagGroupIndex i = 0; i < tag_group_count_; i++) {
    TagGroup *group = tag_groups_[i];
    if (group
	&& group->hasFilterTag()) {
      tag_group_set_->eraseKey(group);
      tag_groups_[group->index()] = NULL;
      delete group;
    }
  }
}

void
Search::deleteFilterTags()
{
  for (TagIndex i = 0; i < tag_count_; i++) {
    Tag *tag = tags_[i];
    if (tag
	&& tag->isFilter()) {
      tags_[i] = NULL;
      tag_set_->eraseKey(tag);
      delete tag;
    }
  }
}

void
Search::deleteFilterClkInfos()
{
  ClkInfoSet::Iterator clk_info_iter(clk_info_set_);
  while (clk_info_iter.hasNext()) {
    ClkInfo *clk_info = clk_info_iter.next();
    if (clk_info->refsFilter(this)) {
      clk_info_set_->eraseKey(clk_info);
      delete clk_info;
    }
  }
}

void
Search::findFilteredArrivals()
{
  findArrivals1();
  seedFilterStarts();
  Level max_level = levelize_->maxLevel();
  // Search always_to_endpoint to search from exisiting arrivals at
  // fanin startpoints to reach -thru/-to endpoints.
  arrival_visitor_->init(true);
  // Iterate until data arrivals at all latches stop changing.
  for (int pass = 1; pass <= 2 || havePendingLatchOutputs() ; pass++) {
    enqueuePendingLatchOutputs();
    debugPrint1(debug_, "search", 1, "find arrivals pass %d\n", pass);
    int arrival_count = arrival_iter_->visitParallel(max_level,
						     arrival_visitor_);
    debugPrint1(debug_, "search", 1, "found %d arrivals\n", arrival_count);
  }
  arrivals_exist_ = true;
}

class SeedFaninsThruHierPin : public HierPinThruVisitor
{
public:
  SeedFaninsThruHierPin(Graph *graph,
			Search *search);

protected:
  virtual void visit(Pin *drvr,
		     Pin *load);

  Graph *graph_;
  Search *search_;

private:
  DISALLOW_COPY_AND_ASSIGN(SeedFaninsThruHierPin);
};

SeedFaninsThruHierPin::SeedFaninsThruHierPin(Graph *graph,
					     Search *search) :
  HierPinThruVisitor(),
  graph_(graph),
  search_(search)
{
}

void
SeedFaninsThruHierPin::visit(Pin *drvr,
			     Pin *)
{
  Vertex *vertex, *bidirect_drvr_vertex;
  graph_->pinVertices(drvr, vertex, bidirect_drvr_vertex);
  search_->seedArrival(vertex);
  if (bidirect_drvr_vertex)
    search_->seedArrival(bidirect_drvr_vertex);
}

void
Search::seedFilterStarts()
{
  ExceptionPt *first_pt = filter_->firstPt();
  PinSet first_pins;
  first_pt->allPins(network_, &first_pins);
  PinSet::Iterator pin_iter(first_pins);
  while (pin_iter.hasNext()) {
    const Pin *pin = pin_iter.next();
    if (network_->isHierarchical(pin)) {
      SeedFaninsThruHierPin visitor(graph_, this);
      visitDrvrLoadsThruHierPin(pin, network_, &visitor);
    }
    else {
      Vertex *vertex, *bidirect_drvr_vertex;
      graph_->pinVertices(pin, vertex, bidirect_drvr_vertex);
      seedArrival(vertex);
      if (bidirect_drvr_vertex)
	seedArrival(bidirect_drvr_vertex);
    }
  }
}

////////////////////////////////////////////////////////////////

void
Search::deleteVertexBefore(Vertex *vertex)
{
  if (arrivals_exist_) {
    deletePaths(vertex);
    arrival_iter_->deleteVertexBefore(vertex);
    invalid_arrivals_.eraseKey(vertex);
  }
  if (requireds_exist_) {
    required_iter_->deleteVertexBefore(vertex);
    invalid_requireds_.eraseKey(vertex);
    invalid_tns_.eraseKey(vertex);
  }
  if (endpoints_)
    endpoints_->eraseKey(vertex);
  if (invalid_endpoints_)
    invalid_endpoints_->eraseKey(vertex);
}

void
Search::arrivalsInvalid()
{
  if (arrivals_exist_) {
    debugPrint0(debug_, "search", 1, "arrivals invalid\n");
    // Delete paths to make sure no state is left over.
    // For example, set_disable_timing strands a vertex, which means
    // the search won't revisit it to clear the previous arrival.
    deletePaths();
    deleteTags();
    genclks_->clear();
    deleteFilter();
    arrivals_exist_ = false;
    arrivals_at_endpoints_exist_ = false;
    arrivals_seeded_ = false;
    requireds_exist_ = false;
    requireds_seeded_ = false;
    clk_arrivals_valid_ = false;
    arrival_iter_->clear();
    required_iter_->clear();
    // No need to keep track of incremental updates any more.
    invalid_arrivals_.clear();
    invalid_requireds_.clear();
    tns_exists_ = false;
    clearWorstSlack();
    invalid_tns_.clear();
  }
}

void
Search::requiredsInvalid()
{
  debugPrint0(debug_, "search", 1, "requireds invalid\n");
  requireds_exist_ = false;
  requireds_seeded_ = false;
  invalid_requireds_.clear();
  tns_exists_ = false;
  clearWorstSlack();
  invalid_tns_.clear();
}

void
Search::arrivalInvalid(Vertex *vertex)
{
  if (arrivals_exist_) {
    debugPrint1(debug_, "search", 2, "arrival invalid %s\n",
		vertex->name(sdc_network_));
    if (!arrival_iter_->inQueue(vertex)) {
      // Lock for DelayCalcObserveRequired called by GraphDelayCalc threads.
      invalid_arrivals_lock_.lock();
      invalid_arrivals_.insert(vertex);
      invalid_arrivals_lock_.unlock();
    }
    tnsInvalid(vertex);
  }
}

void
Search::arrivalInvalidDelete(Vertex *vertex)
{
  arrivalInvalid(vertex);
  deletePaths1(vertex);
}

void
Search::levelChangedBefore(Vertex *vertex)
{
  if (arrivals_exist_) {
    arrival_iter_->remove(vertex);
    required_iter_->remove(vertex);
    search_->arrivalInvalid(vertex);
    search_->requiredInvalid(vertex);
  }
}

void
Search::arrivalInvalid(const Pin *pin)
{
  if (graph_) {
    Vertex *vertex, *bidirect_drvr_vertex;
    graph_->pinVertices(pin, vertex, bidirect_drvr_vertex);
    arrivalInvalid(vertex);
    if (bidirect_drvr_vertex)
      arrivalInvalid(bidirect_drvr_vertex);
  }
}

void
Search::requiredInvalid(Instance *inst)
{
  if (graph_) {
    InstancePinIterator *pin_iter = network_->pinIterator(inst);
    while (pin_iter->hasNext()) {
      Pin *pin = pin_iter->next();
      requiredInvalid(pin);
    }
    delete pin_iter;
  }
}

void
Search::requiredInvalid(const Pin *pin)
{
  if (graph_) {
    Vertex *vertex, *bidirect_drvr_vertex;
    graph_->pinVertices(pin, vertex, bidirect_drvr_vertex);
    requiredInvalid(vertex);
    if (bidirect_drvr_vertex)
      requiredInvalid(bidirect_drvr_vertex);
  }
}

void
Search::requiredInvalid(Vertex *vertex)
{
  if (requireds_exist_) {
    debugPrint1(debug_, "search", 2, "required invalid %s\n",
		vertex->name(sdc_network_));
    if (!required_iter_->inQueue(vertex)) {
      // Lock for DelayCalcObserveRequired called by GraphDelayCalc threads.
      invalid_arrivals_lock_.lock();
      invalid_requireds_.insert(vertex);
      invalid_arrivals_lock_.unlock();
    }
    tnsInvalid(vertex);
  }
}

////////////////////////////////////////////////////////////////

void
Search::findClkArrivals()
{
  if (!clk_arrivals_valid_) {
    genclks_->ensureInsertionDelays();
    Stats stats(debug_);
    debugPrint0(debug_, "search", 1, "find clk arrivals\n");
    arrival_iter_->clear();
    seedClkVertexArrivals();
    ClkArrivalSearchPred search_clk(this);
    arrival_visitor_->init(false, &search_clk);
    arrival_iter_->visitParallel(levelize_->maxLevel(), arrival_visitor_);
    arrivals_exist_ = true;
    stats.report("Find clk arrivals");
  }
  clk_arrivals_valid_ = true;
}

void
Search::seedClkVertexArrivals()
{
  PinSet clk_pins;
  findClkVertexPins(clk_pins);
  PinSet::Iterator pin_iter(clk_pins);
  while (pin_iter.hasNext()) {
    Pin *pin = pin_iter.next();
    Vertex *vertex, *bidirect_drvr_vertex;
    graph_->pinVertices(pin, vertex, bidirect_drvr_vertex);
    seedClkVertexArrivals(pin, vertex);
    if (bidirect_drvr_vertex)
      seedClkVertexArrivals(pin, bidirect_drvr_vertex);
  }
}

void
Search::seedClkVertexArrivals(const Pin *pin,
			      Vertex *vertex)
{
  TagGroupBldr tag_bldr(true, this);
  tag_bldr.init(vertex);
  genclks_->copyGenClkSrcPaths(vertex, &tag_bldr);
  seedClkArrivals(pin, vertex, &tag_bldr);
  setVertexArrivals(vertex, &tag_bldr);
}

Arrival
Search::clockInsertion(const Clock *clk,
		       const Pin *pin,
		       const TransRiseFall *tr,
		       const MinMax *min_max,
		       const EarlyLate *early_late,
		       const PathAnalysisPt *path_ap) const
{
  float insert;
  bool exists;
  sdc_->clockInsertion(clk, pin, tr, min_max, early_late, insert, exists);
  if (exists)
    return insert;
  else if (clk->isGeneratedWithPropagatedMaster())
    return genclks_->insertionDelay(clk, pin, tr, early_late, path_ap);
  else
    return 0.0;
}

////////////////////////////////////////////////////////////////

void
Search::visitStartpoints(VertexVisitor *visitor)
{
  Instance *top_inst = network_->topInstance();
  InstancePinIterator *pin_iter = network_->pinIterator(top_inst);
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    if (network_->direction(pin)->isAnyInput()) {
      Vertex *vertex = graph_->pinDrvrVertex(pin);
      visitor->visit(vertex);
    }
  }
  delete pin_iter;

  InputDelayVertexPinsIterator *arrival_iter =
    sdc_->inputDelayVertexPinsIterator();
  while (arrival_iter->hasNext()) {
    const Pin *pin = arrival_iter->next();
    // Already hit these.
    if (!network_->isTopLevelPort(pin)) {
      Vertex *vertex = graph_->pinDrvrVertex(pin);
      if (vertex)
	visitor->visit(vertex);
    }
  }
  delete arrival_iter;

  ClockIterator *clk_iter = sdc_->clockIterator();
  while (clk_iter->hasNext()) {
    Clock *clk = clk_iter->next();
    ClockVertexPinIterator pin_iter(clk);
    while (pin_iter.hasNext()) {
      Pin *pin = pin_iter.next();
      // Already hit these.
      if (!network_->isTopLevelPort(pin)) {
	Vertex *vertex = graph_->pinDrvrVertex(pin);
	visitor->visit(vertex);
      }
    }
  }
  delete clk_iter;

  // Register clk pins.
  VertexSet::ConstIterator reg_clk_iter(graph_->regClkVertices());
  while (reg_clk_iter.hasNext()) {
    Vertex *vertex = reg_clk_iter.next();
    visitor->visit(vertex);
  }

  PinSet::Iterator path_pin_iter(sdc_->pathDelayInternalStartpoints());
  while (path_pin_iter.hasNext()) {
    Pin *pin = path_pin_iter.next();
    Vertex *vertex = graph_->pinDrvrVertex(pin);
    visitor->visit(vertex);
  }
}

void
Search::visitEndpoints(VertexVisitor *visitor)
{
  VertexSet::Iterator end_iter(endpoints());
  while (end_iter.hasNext()) {
    Vertex *end = end_iter.next();
    Pin *pin = end->pin();
    // Filter register clock pins (fails on set_max_delay -from clk_src).
    if (!network_->isRegClkPin(pin)
	|| sdc_->isPathDelayInternalEndpoint(pin))
      visitor->visit(end);
  }
}

////////////////////////////////////////////////////////////////

void
Search::findAllArrivals()
{
  arrival_visitor_->init(false);
  findAllArrivals(arrival_visitor_);
}

void
Search::findAllArrivals(VertexVisitor *arrival_visitor)
{
  // Iterate until data arrivals at all latches stop changing.
  for (int pass = 1; pass == 1 || havePendingLatchOutputs(); pass++) {
    enqueuePendingLatchOutputs();
    debugPrint1(debug_, "search", 1, "find arrivals pass %d\n", pass);
    findArrivals(levelize_->maxLevel(), arrival_visitor);
  }
}

bool
Search::havePendingLatchOutputs()
{
  return pending_latch_outputs_.size() > 0;
}

void
Search::clearPendingLatchOutputs()
{
  pending_latch_outputs_.clear();
}

void
Search::enqueuePendingLatchOutputs()
{
  VertexSet::Iterator latch_iter(pending_latch_outputs_);
  while (latch_iter.hasNext()) {
    Vertex *latch_vertex = latch_iter.next();
    arrival_iter_->enqueue(latch_vertex);
  }
  clearPendingLatchOutputs();
}

void
Search::findArrivals()
{
  findArrivals(levelize_->maxLevel());
}

void
Search::findArrivals(Level level)
{
  arrival_visitor_->init(false);
  findArrivals(level, arrival_visitor_);
}

void
Search::findArrivals(Level level,
		     VertexVisitor *arrival_visitor)
{
  debugPrint1(debug_, "search", 1, "find arrivals to level %d\n", level);
  findArrivals1();
  Stats stats(debug_);
  int arrival_count = arrival_iter_->visitParallel(level, arrival_visitor);
  stats.report("Find arrivals");
  if (arrival_iter_->empty()
      && invalid_arrivals_.empty()) {
    clk_arrivals_valid_ = true;
    arrivals_at_endpoints_exist_ = true;
  }
  arrivals_exist_ = true;
  debugPrint1(debug_, "search", 1, "found %u arrivals\n", arrival_count);
}

void
Search::findArrivals1()
{
  if (!arrivals_seeded_) {
    genclks_->ensureInsertionDelays();
    arrival_iter_->clear();
    required_iter_->clear();
    seedArrivals();
    arrivals_seeded_ = true;
  }
  else {
    arrival_iter_->ensureSize();
    required_iter_->ensureSize();
  }
  seedInvalidArrivals();
}

////////////////////////////////////////////////////////////////

ArrivalVisitor::ArrivalVisitor(const StaState *sta) :
  PathVisitor(NULL, sta)
{
  init0();
  init(true);
}

// Copy constructor.
ArrivalVisitor::ArrivalVisitor(bool always_to_endpoints,
			       SearchPred *pred,
			       const StaState *sta) :
  PathVisitor(pred, sta)
{
  init0();
  init(always_to_endpoints, pred);
}

void
ArrivalVisitor::init0()
{
  tag_bldr_ = new TagGroupBldr(true, sta_);
  tag_bldr_no_crpr_ = new TagGroupBldr(false, sta_);
  adj_pred_ = new SearchThru(tag_bldr_, sta_);
}

void
ArrivalVisitor::init(bool always_to_endpoints)
{
  Search *search = sta_->search();
  init(always_to_endpoints, search ? search->evalPred() : NULL);
}

void
ArrivalVisitor::init(bool always_to_endpoints,
		     SearchPred *pred)
{
  always_to_endpoints_ = always_to_endpoints;
  pred_ = pred;
  crpr_active_ = sta_->sdc()->crprActive();
}


VertexVisitor *
ArrivalVisitor::copy()
{
  return new ArrivalVisitor(always_to_endpoints_, pred_, sta_);
}

ArrivalVisitor::~ArrivalVisitor()
{
  delete tag_bldr_;
  delete tag_bldr_no_crpr_;
  delete adj_pred_;
}

void
ArrivalVisitor::setAlwaysToEndpoints(bool to_endpoints)
{
  always_to_endpoints_ = to_endpoints;
}

void
ArrivalVisitor::visit(Vertex *vertex)
{
  const Debug *debug = sta_->debug();
  const Network *network = sta_->network();
  const Network *sdc_network = sta_->sdcNetwork();
  const Graph *graph = sta_->graph();
  const Sdc *sdc = sta_->sdc();
  Search *search = sta_->search();
  debugPrint1(debug, "search", 2, "find arrivals %s\n",
	      vertex->name(sdc_network));
  Pin *pin = vertex->pin();
  // Don't clobber clock sources.
  if (!sdc->isVertexPinClock(pin)
      // Unless it is an internal path delay endpoint.
      || sdc->isPathDelayInternalEndpoint(pin)) {
    tag_bldr_->init(vertex);
    has_fanin_one_ = graph->hasFaninOne(vertex);
    if (crpr_active_
	&& !has_fanin_one_)
      tag_bldr_no_crpr_->init(vertex);

    visitFaninPaths(vertex);
    if (crpr_active_
	&& !has_fanin_one_)
      pruneCrprArrivals();

    // Insert paths that originate here but 
    if (!network->isTopLevelPort(pin)
	&& sdc->hasInputDelay(pin))
      // set_input_delay on internal pin.
      search->seedInputSegmentArrival(pin, vertex, tag_bldr_);
    if (sdc->isPathDelayInternalStartpoint(pin))
      // set_min/max_delay on internal pin.
      search->makeUnclkedPaths(vertex, true, tag_bldr_);
    if (sdc->isPathDelayInternalEndpoint(pin)
	&& sdc->isVertexPinClock(pin))
      // set_min/max_delay on internal pin also a clock src. Bizzaroland.
      // Re-seed the clock arrivals on top of the propagated paths.
      search->seedClkArrivals(pin, vertex, tag_bldr_);
    // Register/latch clock pin that is not connected to a declared clock.
    // Seed with unclocked tag, zero arrival and allow search thru reg
    // clk->q edges.
    // These paths are required to report path delays from unclocked registers
    // For example, "set_max_delay -to" from an unclocked source register.
    bool is_clk = tag_bldr_->hasClkTag();
    if (vertex->isRegClk() && !is_clk) {
      debugPrint1(debug, "search", 2, "arrival seed unclked reg clk %s\n",
		  network->pathName(pin));
      search->makeUnclkedPaths(vertex, true, tag_bldr_);
    }

    bool arrivals_changed = search->arrivalsChanged(vertex, tag_bldr_);
    // If vertex is a latch data input arrival that changed from the
    // previous eval pass enqueue the latch outputs to be re-evaled on the
    // next pass.
    if (network->isLatchData(pin)) {
      if (arrivals_changed
	  && network->isLatchData(pin))
	search->enqueueLatchDataOutputs(vertex);
    }
    if ((!search->arrivalsAtEndpointsExist()
	 || always_to_endpoints_
	 || arrivals_changed)
	&& (network->isRegClkPin(pin)
	    || !sdc->isPathDelayInternalEndpoint(pin)))
      search->arrivalIterator()->enqueueAdjacentVertices(vertex, adj_pred_);
    if (arrivals_changed) {
      debugPrint0(debug, "search", 4, "arrival changed\n");
      // Only update arrivals when delays change by more than
      // fuzzyEqual can distinguish.
      search->setVertexArrivals(vertex, tag_bldr_);
      search->tnsInvalid(vertex);
      constrainedRequiredsInvalid(vertex, is_clk);
    }
    enqueueRefPinInputDelays(pin);
  }
}

// When a clock arrival changes, the required time changes for any
// timing checks, data checks or gated clock enables constrained
// by the clock pin.
void
ArrivalVisitor::constrainedRequiredsInvalid(Vertex *vertex,
					    bool is_clk)
{
  Search *search = sta_->search();
  Pin *pin = vertex->pin();
  const Network *network = sta_->network();
  if (network->isLoad(pin)
      && search->requiredsExist()) {
    const Graph *graph = sta_->graph();
    const Sdc *sdc = sta_->sdc();
    if (is_clk && network->isCheckClk(pin)) {
      VertexOutEdgeIterator edge_iter(vertex, graph);
      while (edge_iter.hasNext()) {
	Edge *edge = edge_iter.next();
	if (edge->role()->isTimingCheck()) {
	  Vertex *to_vertex = edge->to(graph);
	  search->requiredInvalid(to_vertex);
	}
      }
    }
    // Data checks (vertex does not need to be a clk).
    DataCheckSet *data_checks = sdc->dataChecksFrom(pin);
    if (data_checks) {
      DataCheckSet::Iterator check_iter(data_checks);
      while (check_iter.hasNext()) {
	DataCheck *data_check = check_iter.next();
	Pin *to = data_check->to();
	search->requiredInvalid(to);
      }
    }
    // Gated clocks.
    if (is_clk && sdc->gatedClkChecksEnabled()) {
      PinSet enable_pins;
      search->gatedClk()->gatedClkEnables(vertex, enable_pins);
      PinSet::Iterator enable_iter(enable_pins);
      while (enable_iter.hasNext()) {
	const Pin *enable = enable_iter.next();
	search->requiredInvalid(enable);
      }
    }
  }
}

bool
Search::arrivalsChanged(Vertex *vertex,
			TagGroupBldr *tag_bldr)
{
  Arrival *arrivals1 = vertex->arrivals();
  if (arrivals1) {
    TagGroup *tag_group = tagGroup(vertex);
    if (tag_group->arrivalMap()->size() != tag_bldr->arrivalMap()->size())
      return true;
    ArrivalMap::Iterator arrival_iter1(tag_group->arrivalMap());
    while (arrival_iter1.hasNext()) {
      Tag *tag1;
      int arrival_index1;
      arrival_iter1.next(tag1, arrival_index1);
      Arrival &arrival1 = arrivals1[arrival_index1];
      Arrival arrival2;
      bool arrival_exists2;
      tag_bldr->tagArrival(tag1, arrival2, arrival_exists2);
      if (!arrival_exists2
	  || !delayFuzzyEqual(arrival1, arrival2))
	return true;
    }
    return false;
  }
  else
    return true;
}

bool
ArrivalVisitor::visitFromToPath(const Pin *,
				Vertex *from_vertex,
				const TransRiseFall *from_tr,
				Tag *from_tag,
				PathVertex *from_path,
				Edge *,
				TimingArc *,
				ArcDelay arc_delay,
				Vertex *,
				const TransRiseFall *to_tr,
				Tag *to_tag,
				Arrival &to_arrival,
				const MinMax *min_max,
				const PathAnalysisPt *)
{
  const Debug *debug = sta_->debug();
  const Network *sdc_network = sta_->sdcNetwork();
  debugPrint1(debug, "search", 3, " %s\n",
	      from_vertex->name(sdc_network));
  debugPrint3(debug, "search", 3, "  %s -> %s %s\n",
	      from_tr->asString(),
	      to_tr->asString(),
	      min_max->asString());
  debugPrint1(debug, "search", 3, "  from tag: %s\n",
	      from_tag->asString(sta_));
  debugPrint1(debug, "search", 3, "  to tag  : %s\n",
	      to_tag->asString(sta_));
  ClkInfo *to_clk_info = to_tag->clkInfo();
  bool to_is_clk = to_tag->isClock();
  Arrival arrival;
  int arrival_index;
  Tag *tag_match;
  tag_bldr_->tagMatchArrival(to_tag, tag_match, arrival, arrival_index);
  if (tag_match == NULL
      || delayFuzzyGreater(to_arrival, arrival, min_max)) {
    debugPrint5(debug, "search", 3, "   %s + %s = %s %s %s\n",
		delayAsString(from_path->arrival(sta_), sta_),
		delayAsString(arc_delay, sta_),
		delayAsString(to_arrival, sta_),
		min_max == MinMax::max() ? ">" : "<",
		tag_match ? delayAsString(arrival, sta_) : "MIA");
    PathVertexRep prev_path;
    if (to_tag->isClock() || to_tag->isGenClkSrcPath())
      prev_path.init(from_path, sta_);
    tag_bldr_->setMatchArrival(to_tag, tag_match,
			       to_arrival, arrival_index,
			       &prev_path);
    if (crpr_active_
	&& !has_fanin_one_
	&& to_clk_info->hasCrprClkPin()
	&& !to_is_clk) {
      tag_bldr_no_crpr_->tagMatchArrival(to_tag, tag_match,
					 arrival, arrival_index);
      if (tag_match == NULL
	  || delayFuzzyGreater(to_arrival, arrival, min_max)) {
	tag_bldr_no_crpr_->setMatchArrival(to_tag, tag_match,
					   to_arrival, arrival_index,
					   &prev_path);
      }
    }
  }
  return true;
}

void
ArrivalVisitor::pruneCrprArrivals()
{
  const Debug *debug = sta_->debug();
  ArrivalMap::Iterator arrival_iter(tag_bldr_->arrivalMap());
  Crpr *crpr = sta_->search()->crpr();
  while (arrival_iter.hasNext()) {
    Tag *tag;
    int arrival_index;
    arrival_iter.next(tag, arrival_index);
    ClkInfo *clk_info = tag->clkInfo();
    if (!tag->isClock()
	&& clk_info->hasCrprClkPin()) {
      PathAnalysisPt *path_ap = tag->pathAnalysisPt(sta_);
      const MinMax *min_max = path_ap->pathMinMax();
      Tag *tag_no_crpr;
      Arrival max_arrival;
      int max_arrival_index;
      tag_bldr_no_crpr_->tagMatchArrival(tag, tag_no_crpr,
					 max_arrival, max_arrival_index);
      if (tag_no_crpr) {
	ClkInfo *clk_info_no_crpr = tag_no_crpr->clkInfo();
	Arrival max_crpr = crpr->maxCrpr(clk_info_no_crpr);
	Arrival max_arrival_max_crpr = (min_max == MinMax::max())
	  ? max_arrival - max_crpr
	  : max_arrival + max_crpr;
	debugPrint4(debug, "search", 4, "  cmp %s %s - %s = %s\n",
		    tag->asString(sta_),
		    delayAsString(max_arrival, sta_),
		    delayAsString(max_crpr, sta_),
		    delayAsString(max_arrival_max_crpr, sta_));
	Arrival arrival = tag_bldr_->arrival(arrival_index);
	if (delayFuzzyGreater(max_arrival_max_crpr, arrival, min_max)) {
	  debugPrint1(debug, "search", 3, "  pruned %s\n",
		      tag->asString(sta_));
	  tag_bldr_->deleteArrival(tag);
	}
      }
    }
  }
}

// Enqueue pins with input delays that use ref_pin as the clock
// reference pin as if there is a timing arc from the reference pin to
// the input delay pin.
void
ArrivalVisitor::enqueueRefPinInputDelays(const Pin *ref_pin)
{
  const Sdc *sdc = sta_->sdc();
  InputDelaySet *input_delays = sdc->refPinInputDelays(ref_pin);
  if (input_delays) {
    const Graph *graph = sta_->graph();
    InputDelaySet::Iterator input_iter(input_delays);
    while (input_iter.hasNext()) {
      InputDelay *input_delay = input_iter.next();
      const Pin *pin = input_delay->pin();
      Vertex *vertex, *bidirect_drvr_vertex;
      graph->pinVertices(pin, vertex, bidirect_drvr_vertex);
      seedInputDelayArrival(pin, vertex, input_delay);
      if (bidirect_drvr_vertex)
	seedInputDelayArrival(pin, bidirect_drvr_vertex, input_delay);
    }
  }
}

void
ArrivalVisitor::seedInputDelayArrival(const Pin *pin,
				      Vertex *vertex,
				      InputDelay *input_delay)
{
  TagGroupBldr tag_bldr(true, sta_);
  Search *search = sta_->search();
  Network *network = sta_->network();
  tag_bldr.init(vertex);
  search->seedInputDelayArrival(pin, vertex, input_delay,
				!network->isTopLevelPort(pin), &tag_bldr);
  search->setVertexArrivals(vertex, &tag_bldr);
  search->arrivalIterator()->enqueueAdjacentVertices(vertex,
						     search->searchAdj());
}

void
Search::enqueueLatchDataOutputs(Vertex *vertex)
{
  VertexOutEdgeIterator out_edge_iter(vertex, graph_);
  while (out_edge_iter.hasNext()) {
    Edge *out_edge = out_edge_iter.next();
    if (latches_->isLatchDtoQ(out_edge)) {
      Vertex *out_vertex = out_edge->to(graph_);
      pending_latch_outputs_lock_.lock();
      pending_latch_outputs_.insert(out_vertex);
      pending_latch_outputs_lock_.unlock();
    }
  }
}

void
Search::seedArrivals()
{
  VertexSet vertices;
  findClockVertices(vertices);
  findRootVertices(vertices);
  findInputDrvrVertices(vertices);

  VertexSet::Iterator vertex_iter(vertices);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    seedArrival(vertex);
  }
}

void
Search::findClockVertices(VertexSet &vertices)
{
  ClockIterator *clk_iter = sdc_->clockIterator();
  while (clk_iter->hasNext()) {
    Clock *clk = clk_iter->next();
    ClockVertexPinIterator pin_iter(clk);
    while (pin_iter.hasNext()) {
      Pin *pin = pin_iter.next();
      Vertex *vertex, *bidirect_drvr_vertex;
      graph_->pinVertices(pin, vertex, bidirect_drvr_vertex);
      vertices.insert(vertex);
      if (bidirect_drvr_vertex)
	vertices.insert(bidirect_drvr_vertex);
    }
  }
  delete clk_iter;
}

void
Search::seedInvalidArrivals()
{
  VertexSet::Iterator vertex_iter(invalid_arrivals_);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    seedArrival(vertex);
  }
  invalid_arrivals_.clear();
}

void
Search::seedArrival(Vertex *vertex)
{
  const Pin *pin = vertex->pin();
  if (sdc_->isVertexPinClock(pin)) {
    TagGroupBldr tag_bldr(true, this);
    tag_bldr.init(vertex);
    genclks_->copyGenClkSrcPaths(vertex, &tag_bldr);
    seedClkArrivals(pin, vertex, &tag_bldr);
    // Clock pin may also have input arrivals from other clocks.
    seedInputArrival(pin, vertex, &tag_bldr);
    setVertexArrivals(vertex, &tag_bldr);
  }
  else if (isInputArrivalSrchStart(vertex)) {
    TagGroupBldr tag_bldr(true, this);
    tag_bldr.init(vertex);
    seedInputArrival(pin, vertex, &tag_bldr);
    setVertexArrivals(vertex, &tag_bldr);
    if (!tag_bldr.empty())
      // Only search downstream if there were non-false paths from here.
      arrival_iter_->enqueueAdjacentVertices(vertex, search_adj_);
  }
  else if (levelize_->isRoot(vertex)) {
    bool is_reg_clk = vertex->isRegClk();
    if (is_reg_clk
	// Internal roots isolated by disabled pins are seeded with no clock.
	|| (report_unconstrained_paths_
	    && !network_->isTopLevelPort(pin))) {
      debugPrint1(debug_, "search", 2, "arrival seed unclked root %s\n",
		  network_->pathName(pin));
      TagGroupBldr tag_bldr(true, this);
      tag_bldr.init(vertex);
      if (makeUnclkedPaths(vertex, is_reg_clk, &tag_bldr))
	// Only search downstream if there were no false paths from here.
	arrival_iter_->enqueueAdjacentVertices(vertex, search_adj_);
      setVertexArrivals(vertex, &tag_bldr);
    }
    else {
      deletePaths(vertex);
      if (search_adj_->searchFrom(vertex))
	arrival_iter_->enqueueAdjacentVertices(vertex,  search_adj_);
    }
  }
  else {
    debugPrint1(debug_, "search", 2, "arrival enqueue %s\n",
		network_->pathName(pin));
    arrival_iter_->enqueue(vertex);
  }
}

// Find all of the clock vertex pins.
void
Search::findClkVertexPins(PinSet &clk_pins)
{
  ClockIterator *clk_iter = sdc_->clockIterator();
  while (clk_iter->hasNext()) {
    Clock *clk = clk_iter->next();
    ClockVertexPinIterator pin_iter(clk);
    while (pin_iter.hasNext()) {
      Pin *pin = pin_iter.next();
      clk_pins.insert(pin);
    }
  }
  delete clk_iter;
}

void
Search::seedClkArrivals(const Pin *pin,
			Vertex *vertex,
			TagGroupBldr *tag_bldr)
{
  ClockSet::Iterator clk_iter(sdc_->findVertexPinClocks(pin));
  while (clk_iter.hasNext()) {
    Clock *clk = clk_iter.next();
    debugPrint2(debug_, "search", 2, "arrival seed clk %s pin %s\n",
		clk->name(), network_->pathName(pin));
    PathAnalysisPtIterator path_ap_iter(this);
    while (path_ap_iter.hasNext()) {
      PathAnalysisPt *path_ap = path_ap_iter.next();
      const MinMax *min_max = path_ap->pathMinMax();
      TransRiseFallIterator tr_iter;
      while (tr_iter.hasNext()) {
	TransRiseFall *tr = tr_iter.next();
	ClockEdge *clk_edge = clk->edge(tr);
	const EarlyLate *early_late = min_max;
	if (clk->isGenerated()
	    && clk->masterClk() == NULL)
	  seedClkDataArrival(pin, tr, clk, clk_edge, min_max, path_ap,
			     0.0, tag_bldr);
	else {
	  Arrival insertion = clockInsertion(clk, pin, tr, min_max,
					     early_late, path_ap);
	  seedClkArrival(pin, tr, clk, clk_edge, min_max, path_ap,
			 insertion, tag_bldr);
	}
      }
    }
    arrival_iter_->enqueueAdjacentVertices(vertex,  search_adj_);
  }
}

void
Search::seedClkArrival(const Pin *pin,
		       const TransRiseFall *tr,
		       Clock *clk,
		       ClockEdge *clk_edge,
		       const MinMax *min_max,
		       const PathAnalysisPt *path_ap,
		       Arrival insertion,
		       TagGroupBldr *tag_bldr)
{
  bool is_propagated = false;
  float latency = 0.0;
  bool latency_exists;
  // Check for clk pin latency.
  sdc_->clockLatency(clk, pin, tr, min_max,
			     latency, latency_exists);
  if (!latency_exists) {
    // Check for clk latency (lower priority).
    sdc_->clockLatency(clk, tr, min_max,
			       latency, latency_exists);
    if (latency_exists) {
      // Propagated pin overrides latency on clk.
      if (sdc_->isPropagatedClock(pin)) {
	latency = 0.0;
	latency_exists = false;
	is_propagated = true;
      }
    }
    else
      is_propagated = sdc_->isPropagatedClock(pin)
	|| clk->isPropagated();
  }

  ClockUncertainties *uncertainties = sdc_->clockUncertainties(pin);
  if (uncertainties == NULL)
    uncertainties = clk->uncertainties();
  // Propagate liberty "pulse_clock" transition to transitive fanout.
  LibertyPort *port = network_->libertyPort(pin);
  TransRiseFall *pulse_clk_sense = (port ? port->pulseClkSense() : NULL);
  ClkInfo *clk_info = findClkInfo(clk_edge, pin, is_propagated, NULL, false,
				  pulse_clk_sense, insertion, latency,
				  uncertainties, path_ap, NULL);
  // Only false_paths -from apply to clock tree pins.
  ExceptionStateSet *states = NULL;
  sdc_->exceptionFromClkStates(pin,tr,clk,tr,min_max,states);
  Tag *tag = findTag(tr, path_ap, clk_info, true, NULL, false, states, true);
  Arrival arrival(clk_edge->time() + insertion);
  tag_bldr->setArrival(tag, arrival, NULL);
}

void
Search::seedClkDataArrival(const Pin *pin,
			   const TransRiseFall *tr,
			   Clock *clk,
			   ClockEdge *clk_edge,
			   const MinMax *min_max,
			   const PathAnalysisPt *path_ap,
			   Arrival insertion,
			   TagGroupBldr *tag_bldr)
{	
  Tag *tag = clkDataTag(pin, clk, tr, clk_edge, insertion, min_max, path_ap);
  if (tag) {
    // Data arrivals include insertion delay.
    Arrival arrival(clk_edge->time() + insertion);
    tag_bldr->setArrival(tag, arrival, NULL);
  }
}

Tag *
Search::clkDataTag(const Pin *pin,
		   Clock *clk,
		   const TransRiseFall *tr,
		   ClockEdge *clk_edge,
		   Arrival insertion,
 		   const MinMax *min_max,
 		   const PathAnalysisPt *path_ap)
{
  ExceptionStateSet *states = NULL;
  if (sdc_->exceptionFromStates(pin, tr, clk, tr, min_max, states)) {
    bool is_propagated = (clk->isPropagated()
			  || sdc_->isPropagatedClock(pin));
    ClkInfo *clk_info = findClkInfo(clk_edge, pin, is_propagated,
				    insertion, path_ap);
    return findTag(tr, path_ap, clk_info, false, NULL, false, states, true);
  }
  else
    return NULL;
}

////////////////////////////////////////////////////////////////

bool
Search::makeUnclkedPaths(Vertex *vertex,
			 bool is_segment_start,
			 TagGroupBldr *tag_bldr)
{
  bool search_from = false;
  const Pin *pin = vertex->pin();
  PathAnalysisPtIterator path_ap_iter(this);
  while (path_ap_iter.hasNext()) {
    PathAnalysisPt *path_ap = path_ap_iter.next();
    const MinMax *min_max = path_ap->pathMinMax();
    TransRiseFallIterator tr_iter;
    while (tr_iter.hasNext()) {
      TransRiseFall *tr = tr_iter.next();
      Tag *tag = fromUnclkedInputTag(pin, tr, min_max, path_ap,
				     is_segment_start);
      if (tag) {
	tag_bldr->setArrival(tag, delay_zero, NULL);
	search_from = true;
      }
    }
  }
  return search_from;
}

// Find graph roots and input ports that do NOT have arrivals.
void
Search::findRootVertices(VertexSet &vertices)
{
  VertexSet::Iterator root_iter(levelize_->roots());
  while (root_iter.hasNext()) {
    Vertex *vertex = root_iter.next();
    const Pin *pin = vertex->pin();
    if (!sdc_->isVertexPinClock(pin)
	&& !sdc_->hasInputDelay(pin)
	&& !vertex->isConstant()) {
      vertices.insert(vertex);
    }
  }
}

void
Search::findInputDrvrVertices(VertexSet &vertices)
{
  Instance *top_inst = network_->topInstance();
  InstancePinIterator *pin_iter = network_->pinIterator(top_inst);
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    if (network_->direction(pin)->isAnyInput())
      vertices.insert(graph_->pinDrvrVertex(pin));
  }
  delete pin_iter;
}

bool
Search::isSegmentStart(const Pin *pin)
{
  return (sdc_->isPathDelayInternalStartpoint(pin)
	  || sdc_->isInputDelayInternal(pin))
    && !sdc_->isVertexPinClock(pin);
}

bool
Search::isInputArrivalSrchStart(Vertex *vertex)
{
  const Pin *pin = vertex->pin();
  PortDirection *dir = network_->direction(pin);
  bool is_top_level_port = network_->isTopLevelPort(pin);
  return (is_top_level_port
	  && (dir->isInput()
	      || (dir->isBidirect() && vertex->isBidirectDriver()))) ;
}

// Seed input arrivals clocked by clks.
void
Search::seedInputArrivals(ClockSet *clks)
{
  // Input arrivals can be on internal pins, so iterate over the pins
  // that have input arrivals rather than the top level input pins.
  InputDelayVertexPinsIterator *arrival_iter =
    sdc_->inputDelayVertexPinsIterator();
  while (arrival_iter->hasNext()) {
    const Pin *pin = arrival_iter->next();
    if (!sdc_->isVertexPinClock(pin)) {
      Vertex *vertex = graph_->pinDrvrVertex(pin);
      seedInputArrival(pin, vertex, clks);
    }
  }
  delete arrival_iter;
}

void
Search::seedInputArrival(const Pin *pin,
			 Vertex *vertex,
			 ClockSet *wrt_clks)
{
  bool has_arrival = false;
  // There can be multiple arrivals for a pin with wrt different clocks.
  PinInputDelayIterator *arrival_iter =
    sdc_->inputDelayVertexIterator(pin);
  TagGroupBldr tag_bldr(true, this);
  tag_bldr.init(vertex);
  while (arrival_iter->hasNext()) {
    InputDelay *input_delay = arrival_iter->next();
    Clock *input_clk = input_delay->clock();
    ClockSet *pin_clks = sdc_->findVertexPinClocks(pin);
    if (input_clk && wrt_clks->hasKey(input_clk)
	// Input arrivals wrt a clock source pin is the insertion
	// delay (source latency), but arrivals wrt other clocks
	// propagate.
	&& (pin_clks == NULL
	    || !pin_clks->hasKey(input_clk))) {
      seedInputDelayArrival(pin, vertex, input_delay, false, &tag_bldr);
      has_arrival = true;
    }
  }
  if (has_arrival)
    setVertexArrivals(vertex, &tag_bldr);
  delete arrival_iter;
}

void
Search::seedInputArrival(const Pin *pin,
			 Vertex *vertex,
			 TagGroupBldr *tag_bldr)
{
  if (sdc_->hasInputDelay(pin))
    seedInputArrival1(pin, vertex, false, tag_bldr);
  else if (!sdc_->isVertexPinClock(pin))
    // Seed inputs without set_input_delays.
    seedInputDelayArrival(pin, vertex, NULL, false, tag_bldr);
}

void
Search::seedInputSegmentArrival(const Pin *pin,
				Vertex *vertex,
				TagGroupBldr *tag_bldr)
{
  seedInputArrival1(pin, vertex, true, tag_bldr);
}

void
Search::seedInputArrival1(const Pin *pin,
			  Vertex *vertex,
			  bool is_segment_start,
			  TagGroupBldr *tag_bldr)
{
  // There can be multiple arrivals for a pin with wrt different clocks.
  PinInputDelayIterator *arrival_iter=
    sdc_->inputDelayVertexIterator(pin);
  while (arrival_iter->hasNext()) {
    InputDelay *input_delay = arrival_iter->next();
    Clock *input_clk = input_delay->clock();
    ClockSet *pin_clks = sdc_->findVertexPinClocks(pin);
    // Input arrival wrt a clock source pin is the clock insertion
    // delay (source latency), but arrivals wrt other clocks
    // propagate.
    if (pin_clks == NULL
	|| !pin_clks->hasKey(input_clk))
      seedInputDelayArrival(pin, vertex, input_delay, is_segment_start,
			    tag_bldr);
  }
  delete arrival_iter;
}

void
Search::seedInputDelayArrival(const Pin *pin,
			      Vertex *vertex,
			      InputDelay *input_delay,
			      bool is_segment_start,
			      TagGroupBldr *tag_bldr)
{
  debugPrint1(debug_, "search", 2,
	      input_delay
	      ? "arrival seed input arrival %s\n"
	      : "arrival seed input %s\n",
	      vertex->name(sdc_network_));
  ClockEdge *clk_edge = NULL;
  const Pin *ref_pin = NULL;
  if (input_delay) {
    clk_edge = input_delay->clkEdge();
    if (clk_edge == NULL
	&& sdc_->useDefaultArrivalClock())
      clk_edge = sdc_->defaultArrivalClockEdge();
    ref_pin = input_delay->refPin();
  }
  else if (sdc_->useDefaultArrivalClock())
    clk_edge = sdc_->defaultArrivalClockEdge();
  if (ref_pin) {
    Vertex *ref_vertex = graph_->pinLoadVertex(ref_pin);
    PathAnalysisPtIterator path_ap_iter(this);
    while (path_ap_iter.hasNext()) {
      PathAnalysisPt *path_ap = path_ap_iter.next();
      const MinMax *min_max = path_ap->pathMinMax();
      TransRiseFall *ref_tr = input_delay->refTransition();
      const Clock *clk = input_delay->clock();
      VertexPathIterator ref_path_iter(ref_vertex, ref_tr, path_ap, this);
      while (ref_path_iter.hasNext()) {
	Path *ref_path = ref_path_iter.next();
	if (ref_path->isClock(this)
	    && (clk == NULL
		|| ref_path->clock(this) == clk)) {
	  float ref_arrival, ref_insertion, ref_latency;
	  inputDelayRefPinArrival(ref_path, ref_path->clkEdge(this), min_max,
				  ref_arrival, ref_insertion, ref_latency);
	  seedInputDelayArrival(pin, input_delay, ref_path->clkEdge(this),
				ref_arrival, ref_insertion, ref_latency,
				is_segment_start, min_max, path_ap, tag_bldr);
	}
      }
    }
  }
  else {
    PathAnalysisPtIterator path_ap_iter(this);
    while (path_ap_iter.hasNext()) {
      PathAnalysisPt *path_ap = path_ap_iter.next();
      const MinMax *min_max = path_ap->pathMinMax();
      float clk_arrival, clk_insertion, clk_latency;
      inputDelayClkArrival(input_delay, clk_edge, min_max, path_ap,
			   clk_arrival, clk_insertion, clk_latency);
      seedInputDelayArrival(pin, input_delay, clk_edge,
			    clk_arrival, clk_insertion, clk_latency,
			    is_segment_start, min_max, path_ap, tag_bldr);
    }
  }
}

// Input delays with -reference_pin use the clock network latency
// from the clock source to the reference pin.
void
Search::inputDelayRefPinArrival(Path *ref_path,
				ClockEdge *clk_edge,
				const MinMax *min_max,
				// Return values.
				float &ref_arrival,
				float &ref_insertion,
				float &ref_latency)
{
  Clock *clk = clk_edge->clock();
  if (clk->isPropagated()) {
    ClkInfo *clk_info = ref_path->clkInfo(this);
    ref_arrival = delayAsFloat(ref_path->arrival(this));
    ref_insertion = delayAsFloat(clk_info->insertion());
    ref_latency = clk_info->latency();
  }
  else {
    const TransRiseFall *clk_tr = clk_edge->transition();
    const EarlyLate *early_late = min_max;
    // Input delays from ideal clk reference pins include clock
    // insertion delay but not latency.
    ref_insertion = sdc_->clockInsertion(clk, clk_tr, min_max,
						 early_late);
    ref_arrival = clk_edge->time() + ref_insertion;
    ref_latency = 0.0;
  }
}

void
Search::seedInputDelayArrival(const Pin *pin,
			      InputDelay *input_delay,
			      ClockEdge *clk_edge,
			      float clk_arrival,
			      float clk_insertion,
			      float clk_latency,
			      bool is_segment_start,
			      const MinMax *min_max,
			      PathAnalysisPt *path_ap,
			      TagGroupBldr *tag_bldr)
{
  TransRiseFallIterator tr_iter;
  while (tr_iter.hasNext()) {
    TransRiseFall *tr = tr_iter.next();
    if (input_delay) {
      float delay;
      bool exists;
      input_delay->delays()->value(tr, min_max, delay, exists);
      if (exists)
	seedInputDelayArrival(pin, tr, clk_arrival + delay,
			      input_delay, clk_edge,
			      clk_insertion,  clk_latency, is_segment_start,
			      min_max, path_ap, tag_bldr);
    }
    else
      seedInputDelayArrival(pin, tr, 0.0,  NULL, clk_edge,
			    clk_insertion,  clk_latency, is_segment_start,
			    min_max, path_ap, tag_bldr);
  }
}

void
Search::seedInputDelayArrival(const Pin *pin,
			      const TransRiseFall *tr,
			      float arrival,
			      InputDelay *input_delay,
			      ClockEdge *clk_edge,
			      float clk_insertion,
			      float clk_latency,
			      bool is_segment_start,
			      const MinMax *min_max,
			      PathAnalysisPt *path_ap,
			      TagGroupBldr *tag_bldr)
{
  Tag *tag = inputDelayTag(pin, tr, clk_edge, clk_insertion, clk_latency,
			   input_delay, is_segment_start, min_max, path_ap);
  if (tag)
    tag_bldr->setArrival(tag, arrival, NULL);
}

void
Search::inputDelayClkArrival(InputDelay *input_delay,
			     ClockEdge *clk_edge,
			     const MinMax *min_max,
			     const PathAnalysisPt *path_ap,
			     // Return values.
			     float &clk_arrival, float &clk_insertion,
			     float &clk_latency)
{
  clk_arrival = 0.0;
  clk_insertion = 0.0;
  clk_latency = 0.0;
  if (input_delay && clk_edge) {
    clk_arrival = clk_edge->time();
    Clock *clk = clk_edge->clock();
    TransRiseFall *clk_tr = clk_edge->transition();
    if (!input_delay->sourceLatencyIncluded()) {
      const EarlyLate *early_late = min_max;
      clk_insertion = delayAsFloat(clockInsertion(clk, clk->defaultPin(),
						  clk_tr, min_max, early_late,
						  path_ap));
      clk_arrival += clk_insertion;
    }
    if (!clk->isPropagated()
	&& !input_delay->networkLatencyIncluded()) {
      clk_latency = sdc_->clockLatency(clk, clk_tr, min_max);
      clk_arrival += clk_latency;
    }
  }
}

Tag *
Search::inputDelayTag(const Pin *pin,
		      const TransRiseFall *tr,
		      ClockEdge *clk_edge,
		      float clk_insertion,
		      float clk_latency,
		      InputDelay *input_delay,
		      bool is_segment_start,
		      const MinMax *min_max,
		      const PathAnalysisPt *path_ap)
{
  Clock *clk = NULL;
  Pin *clk_pin = NULL;
  TransRiseFall *clk_tr = NULL;
  bool is_propagated = false;
  ClockUncertainties *clk_uncertainties = NULL;
  if (clk_edge) {
    clk = clk_edge->clock();
    clk_tr = clk_edge->transition();
    clk_pin = clk->defaultPin();
    is_propagated = clk->isPropagated();
    clk_uncertainties = clk->uncertainties();
  }

  ExceptionStateSet *states = NULL;
  Tag *tag = NULL;
  if (sdc_->exceptionFromStates(pin,tr,clk,clk_tr,min_max,states)) {
    ClkInfo *clk_info = findClkInfo(clk_edge, clk_pin, is_propagated, NULL,
				    false, NULL, clk_insertion, clk_latency,
				    clk_uncertainties, path_ap, NULL);
    tag = findTag(tr, path_ap, clk_info, false, input_delay, is_segment_start,
		  states, true);
  }

  if (tag) {
    ClkInfo *clk_info = tag->clkInfo();
    // Check for state changes on existing tag exceptions (pending -thru pins).
    tag = mutateTag(tag, pin, tr, false, clk_info,
		    pin, tr, false, false, is_segment_start, clk_info,
		    input_delay, min_max, path_ap);
  }
  return tag;
}

////////////////////////////////////////////////////////////////

PathVisitor::PathVisitor(const StaState *sta) :
  pred_(sta->search()->evalPred()),
  sta_(sta)
{
}

PathVisitor::PathVisitor(SearchPred *pred,
			 const StaState *sta) :
  pred_(pred),
  sta_(sta)
{
}

void
PathVisitor::visitFaninPaths(Vertex *to_vertex)
{
  if (pred_->searchTo(to_vertex)) {
    const Graph *graph = sta_->graph();
    VertexInEdgeIterator edge_iter(to_vertex, graph);
    while (edge_iter.hasNext()) {
      Edge *edge = edge_iter.next();
      Vertex *from_vertex = edge->from(graph);
      const Pin *from_pin = from_vertex->pin();
      if (pred_->searchFrom(from_vertex)
	  && pred_->searchThru(edge)) {
	const Pin *to_pin = to_vertex->pin();
	if (!visitEdge(from_pin, from_vertex, edge, to_pin, to_vertex))
	  break;
      }
    }
  }
}

void
PathVisitor::visitFanoutPaths(Vertex *from_vertex)
{
  const Pin *from_pin = from_vertex->pin();
  if (pred_->searchFrom(from_vertex)) {
    const Graph *graph = sta_->graph();
    VertexOutEdgeIterator edge_iter(from_vertex, graph);
    while (edge_iter.hasNext()) {
      Edge *edge = edge_iter.next();
      Vertex *to_vertex = edge->to(graph);
      const Pin *to_pin = to_vertex->pin();
      if (pred_->searchTo(to_vertex)
	  && pred_->searchThru(edge)) {
	debugPrint1(sta_->debug(), "search", 3,
		    " %s\n", to_vertex->name(sta_->network()));
	if (!visitEdge(from_pin, from_vertex, edge, to_pin, to_vertex))
	  break;
      }
    }
  }
}

bool
PathVisitor::visitEdge(const Pin *from_pin,
		       Vertex *from_vertex,
		       Edge *edge,
		       const Pin *to_pin,
		       Vertex *to_vertex)
{
  Search *search = sta_->search();
  TagGroup *from_tag_group = search->tagGroup(from_vertex);
  if (from_tag_group) {
    TimingArcSet *arc_set = edge->timingArcSet();
    VertexPathIterator from_iter(from_vertex, search);
    while (from_iter.hasNext()) {
      PathVertex *from_path = from_iter.next();
      Tag *from_tag = from_path->tag(sta_);
      // Only propagate seeded paths from segment startpoint.
      if (!search->isSegmentStart(from_pin)
	  || from_tag->isSegmentStart()) {
	PathAnalysisPt *path_ap = from_path->pathAnalysisPt(sta_);
	const MinMax *min_max = path_ap->pathMinMax();
	const TransRiseFall *from_tr = from_path->transition(sta_);
	// Do not propagate paths from a clock source unless they are
	// defined on the from pin.
	if (!search->pathPropagatedToClkSrc(from_pin, from_path)) {
	  TimingArc *arc1, *arc2;
	  arc_set->arcsFrom(from_tr, arc1, arc2);
	  if (!visitArc(from_pin, from_vertex, from_tr, from_path,
			edge, arc1, to_pin, to_vertex,
			min_max, path_ap))
	    return false;
	  if (!visitArc(from_pin, from_vertex, from_tr, from_path,
			edge, arc2, to_pin, to_vertex,
			min_max, path_ap))
	    return false;
	}
      }
    }
  }
  return true;
}

bool
PathVisitor::visitArc(const Pin *from_pin,
		      Vertex *from_vertex,
		      const TransRiseFall *from_tr,
		      PathVertex *from_path,
		      Edge *edge,
		      TimingArc *arc,
		      const Pin *to_pin,
		      Vertex *to_vertex,
		      const MinMax *min_max,
		      PathAnalysisPt *path_ap)
{
  if (arc) {
    TransRiseFall *to_tr = arc->toTrans()->asRiseFall();
    if (searchThru(from_vertex, from_tr, edge, to_vertex, to_tr))
      return visitFromPath(from_pin, from_vertex, from_tr, from_path,
			   edge, arc, to_pin, to_vertex, to_tr,
			   min_max, path_ap);
  }
  return true;
}

bool
Search::pathPropagatedToClkSrc(const Pin *pin,
			       Path *path)
{
  const Tag *tag = path->tag(this);
  if (!tag->isGenClkSrcPath()
      // Clock source can have input arrivals from unrelated clock.
      && tag->inputDelay() == NULL
      && sdc_->isPathDelayInternalEndpoint(pin)) {
    ClockSet *clks = sdc_->findVertexPinClocks(pin);
    return clks
      && !clks->hasKey(tag->clock());
  }
  else
    return false;
}

bool
PathVisitor::visitFromPath(const Pin *from_pin,
			   Vertex *from_vertex,
			   const TransRiseFall *from_tr,
			   PathVertex *from_path,
			   Edge *edge,
			   TimingArc *arc,
			   const Pin *to_pin,
			   Vertex *to_vertex,
			   const TransRiseFall *to_tr,
			   const MinMax *min_max,
			   const PathAnalysisPt *path_ap)
{
  Network *network = sta_->network();
  Sdc *sdc = sta_->sdc();
  Search *search = sta_->search();
  Latches *latches = sta_->latches();
  const TimingRole *role = edge->role();
  Tag *from_tag = from_path->tag(sta_);
  ClkInfo *from_clk_info = from_tag->clkInfo();
  Tag *to_tag = NULL;
  ClockEdge *clk_edge = from_clk_info->clkEdge();
  Clock *clk = from_clk_info->clock();
  Arrival from_arrival = from_path->arrival(sta_);
  ArcDelay arc_delay = 0.0;
  Arrival to_arrival;
  if (from_clk_info->isGenClkSrcPath()) {
    if (!sdc->clkStopPropagation(clk,from_pin,from_tr,to_pin,to_tr)
	&& (sdc->clkThruTristateEnabled()
	    || !(role == TimingRole::tristateEnable()
		 || role == TimingRole::tristateDisable()))) {
      Clock *gclk = from_tag->genClkSrcPathClk(sta_);
      if (gclk) {
	Genclks *genclks = search->genclks();
	VertexSet *fanins = genclks->fanins(gclk);
	// Note: encountering a latch d->q edge means find the
	// latch feedback edges, but they are referenced for 
	// other edges in the gen clk fanout.
	if (role == TimingRole::latchDtoQ())
	  genclks->findLatchFdbkEdges(gclk);
	EdgeSet *fdbk_edges = genclks->latchFdbkEdges(gclk);
	if ((role == TimingRole::combinational()
	     || role == TimingRole::wire()
	     || !gclk->combinational())
	    && fanins->hasKey(to_vertex)
	    && !(fdbk_edges && fdbk_edges->hasKey(edge))) {
	  to_tag = search->thruClkTag(from_path, from_tag, true, edge, to_tr,
				      min_max, path_ap);
	  if (to_tag) {
	    arc_delay = search->deratedDelay(from_vertex, arc, edge, true,
					     path_ap);
	    to_arrival = from_arrival + arc_delay;
	  }
	}
      }
      else {
	// PLL out to feedback path.
	to_tag = search->thruTag(from_tag, edge, to_tr, min_max, path_ap);
	if (to_tag) {
	  arc_delay = search->deratedDelay(from_vertex, arc, edge, true,
					   path_ap);
	  to_arrival = from_arrival + arc_delay;
	}
      }
    }
  }
  else if (role->genericRole() == TimingRole::regClkToQ()) {
    if (clk == NULL
	|| !sdc->clkStopPropagation(from_pin, clk)) {
      arc_delay = search->deratedDelay(from_vertex, arc, edge, false, path_ap);
      // Propagate from unclocked reg/latch clk pins, which have no
      // clk but are distinguished with a segment_start flag.
      if ((clk_edge == NULL
	   && from_tag->isSegmentStart())
	  // Do not propagate paths from input ports with default
	  // input arrival clk thru CLK->Q edges.
	  || (clk != sdc->defaultArrivalClock()
	      // Only propagate paths from clocks that have not
	      // passed thru reg/latch D->Q edges.
	      && from_tag->isClock())) {
	const TransRiseFall *clk_tr = clk_edge ? clk_edge->transition() : NULL;
	ClkInfo *to_clk_info = from_clk_info;
	if (network->direction(to_pin)->isInternal())
	  to_clk_info = search->clkInfoWithCrprClkPath(from_clk_info,
						       from_path, path_ap);
	to_tag = search->fromRegClkTag(from_pin, from_tr, clk, clk_tr,
				       to_clk_info, to_pin, to_tr, min_max,
				       path_ap);
	if (to_tag)
	  to_tag = search->thruTag(to_tag, edge, to_tr, min_max, path_ap);
	from_arrival = search->clkPathArrival(from_path, from_clk_info,
					      clk_edge, min_max, path_ap);
	to_arrival = from_arrival + arc_delay;
      }
      else
	to_tag = NULL;
    }
  }
  else if (edge->role() == TimingRole::latchDtoQ()) {
    if (min_max == MinMax::max()) {
      arc_delay = search->deratedDelay(from_vertex, arc, edge, false, path_ap);
      latches->latchOutArrival(from_path, arc, edge, path_ap,
			       to_tag, arc_delay, to_arrival);
      if (to_tag)
	to_tag = search->thruTag(to_tag, edge, to_tr, min_max, path_ap);
    }
  }
  else if (from_tag->isClock()) {
    // Disable edges from hierarchical clock source pins that do
    // not go thru the hierarchical pin and edges from clock source pins
    // that traverse a hierarchical source pin of a different clock.
    // Clock arrivals used as data also need to be disabled.
    if (!(role == TimingRole::wire()
	  && sdc->clkDisabledByHpinThru(clk, from_pin, to_pin))) {
      // Propagate arrival as non-clock at the end of the clock tree.
      bool to_propagates_clk =
	!sdc->clkStopPropagation(clk,from_pin,from_tr,to_pin,to_tr)
	&& (sdc->clkThruTristateEnabled()
	    || !(role == TimingRole::tristateEnable()
		 || role == TimingRole::tristateDisable()));
      arc_delay = search->deratedDelay(from_vertex, arc, edge,
				       to_propagates_clk, path_ap);
      to_tag = search->thruClkTag(from_path, from_tag, to_propagates_clk,
				  edge, to_tr, min_max, path_ap);
      to_arrival = from_arrival + arc_delay;
    }
  }
  else {
    arc_delay = search->deratedDelay(from_vertex, arc, edge, false, path_ap);
    if (!delayFuzzyEqual(arc_delay, min_max->initValue())) {
      to_arrival = from_arrival + arc_delay;
      to_tag = search->thruTag(from_tag, edge, to_tr, min_max, path_ap);
    }
  }
  if (to_tag)
    return visitFromToPath(from_pin, from_vertex, from_tr, from_tag, from_path,
			   edge, arc, arc_delay,
			   to_vertex, to_tr, to_tag, to_arrival,
			   min_max, path_ap);
  else
    return true;
}

Arrival
Search::clkPathArrival(const Path *clk_path) const
{
  ClkInfo *clk_info = clk_path->clkInfo(this);
  ClockEdge *clk_edge = clk_info->clkEdge();
  const PathAnalysisPt *path_ap = clk_path->pathAnalysisPt(this);
  const MinMax *min_max = path_ap->pathMinMax();
  return clkPathArrival(clk_path, clk_info, clk_edge, min_max, path_ap);
}

Arrival
Search::clkPathArrival(const Path *clk_path,
		       ClkInfo *clk_info,
		       ClockEdge *clk_edge,
		       const MinMax *min_max,
		       const PathAnalysisPt *path_ap) const
{
  if (clk_path->vertex(this)->isRegClk()
      && clk_path->isClock(this)
      && clk_edge
      && !clk_info->isPropagated()) {
    // Ideal clock, apply ideal insertion delay and latency.
    const EarlyLate *early_late = min_max;
    return clk_edge->time()
      + clockInsertion(clk_edge->clock(),
		       clk_info->clkSrc(),
		       clk_edge->transition(),
		       min_max, early_late, path_ap)
      + clk_info->latency();
  }
  else
    return clk_path->arrival(this);
}

float
Search::pathClkPathArrival(const Path *path) const
{
  PathRef src_clk_path;
  pathClkPathArrival1(path, src_clk_path);
  if (!src_clk_path.isNull())
    return clkPathArrival(&src_clk_path);
  else
    return 0.0;
}

// PathExpanded::expand() and PathExpanded::clkPath().
void
Search::pathClkPathArrival1(const Path *path,
			    // Return value.
			    PathRef &clk_path) const
{
  PathRef p(path);
  while (!p.isNull()) {
    PathRef prev_path;
    TimingArc *prev_arc;
    p.prevPath(this, prev_path, prev_arc);

    if (p.isClock(this)) {
      clk_path.init(p);
      return;
    }
    if (prev_arc) {
      TimingRole *prev_role = prev_arc->role();
      if (prev_role == TimingRole::regClkToQ()
	  || prev_role == TimingRole::latchEnToQ()) {
	p.prevPath(this, prev_path, prev_arc);
	clk_path.init(prev_path);
	return;
      }
      else if (prev_role == TimingRole::latchDtoQ()) {
	Edge *prev_edge = p.prevEdge(prev_arc, this);
	PathVertex enable_path;
	latches_->latchEnablePath(&p, prev_edge, enable_path);
	clk_path.init(enable_path);
	return;
      }
    }
    p.init(prev_path);
  }
}

////////////////////////////////////////////////////////////////

// Find tag for a path starting with pin/clk_edge.
// Return NULL if a false path starts at pin/clk_edge.
Tag *
Search::fromUnclkedInputTag(const Pin *pin,
			    const TransRiseFall *tr,
			    const MinMax *min_max,
			    const PathAnalysisPt *path_ap,
			    bool is_segment_start)
{
  ExceptionStateSet *states = NULL;
  if (sdc_->exceptionFromStates(pin, tr, NULL, NULL, min_max, states)) {
    ClkInfo *clk_info = findClkInfo(NULL, NULL, false, 0.0, path_ap);
    return findTag(tr, path_ap, clk_info, false, NULL,
		   is_segment_start, states, true);
  }
  else
    return NULL;
}

Tag *
Search::fromRegClkTag(const Pin *from_pin,
		      const TransRiseFall *from_tr,
		      Clock *clk,
		      const TransRiseFall *clk_tr,
		      ClkInfo *clk_info,
		      const Pin *to_pin,
		      const TransRiseFall *to_tr,
		      const MinMax *min_max,
		      const PathAnalysisPt *path_ap)
{
  ExceptionStateSet *states = NULL;
  if (sdc_->exceptionFromStates(from_pin, from_tr, clk, clk_tr,
					min_max, states)) {
    // Hack for filter -from reg/Q.
    sdc_->filterRegQStates(to_pin, to_tr, min_max, states);
    return findTag(to_tr, path_ap, clk_info, false, NULL, false, states, true);
  }
  else
    return NULL;
}

// Insert from_path as ClkInfo crpr_clk_path.
ClkInfo *
Search::clkInfoWithCrprClkPath(ClkInfo *from_clk_info,
			       PathVertex *from_path,
			       const PathAnalysisPt *path_ap)
{
  if (sdc_->crprActive())
    return findClkInfo(from_clk_info->clkEdge(),
		       from_clk_info->clkSrc(),
		       from_clk_info->isPropagated(),
		       from_clk_info->genClkSrc(),
		       from_clk_info->isGenClkSrcPath(),
		       from_clk_info->pulseClkSense(),
		       from_clk_info->insertion(),
		       from_clk_info->latency(),
		       from_clk_info->uncertainties(),
		       path_ap, from_path);
  else
    return from_clk_info;
}

// Find tag for a path starting with from_tag going thru edge.
// Return NULL if the result tag completes a false path.
Tag *
Search::thruTag(Tag *from_tag,
		Edge *edge,
		const TransRiseFall *to_tr,
		const MinMax *min_max,
		const PathAnalysisPt *path_ap)
{
  const Pin *from_pin = edge->from(graph_)->pin();
  Vertex *to_vertex = edge->to(graph_);
  const Pin *to_pin = to_vertex->pin();
  const TransRiseFall *from_tr = from_tag->transition();
  ClkInfo *from_clk_info = from_tag->clkInfo();
  bool to_is_reg_clk = to_vertex->isRegClk();
  Tag *to_tag = mutateTag(from_tag, from_pin, from_tr, false, from_clk_info,
			  to_pin, to_tr, false, to_is_reg_clk, false,
			  // input delay is not propagated.
			  from_clk_info, NULL, min_max, path_ap);
  return to_tag;
}

Tag *
Search::thruClkTag(PathVertex *from_path,
		   Tag *from_tag,
		   bool to_propagates_clk,
		   Edge *edge,
		   const TransRiseFall *to_tr,
		   const MinMax *min_max,
		   const PathAnalysisPt *path_ap)
{
  const Pin *from_pin = edge->from(graph_)->pin();
  Vertex *to_vertex = edge->to(graph_);
  const Pin *to_pin = to_vertex->pin();
  const TransRiseFall *from_tr = from_tag->transition();
  ClkInfo *from_clk_info = from_tag->clkInfo();
  bool from_is_clk = from_tag->isClock();
  bool to_is_reg_clk = to_vertex->isRegClk();
  TimingRole *role = edge->role();
  bool to_is_clk = (from_is_clk
		    && to_propagates_clk
		    && (role->isWire()
			|| role == TimingRole::combinational()));
  ClkInfo *to_clk_info = thruClkInfo(from_path, from_clk_info,
				     edge, to_vertex, to_pin, min_max, path_ap);
  Tag *to_tag = mutateTag(from_tag,from_pin,from_tr,from_is_clk,from_clk_info,
			  to_pin, to_tr, to_is_clk, to_is_reg_clk, false,
			  to_clk_info, NULL, min_max, path_ap);
  return to_tag;
}

// thruTag for clocks.
ClkInfo *
Search::thruClkInfo(PathVertex *from_path,
		    ClkInfo *from_clk_info,
		    Edge *edge,
		    Vertex *to_vertex,
		    const Pin *to_pin,
		    const MinMax *min_max,
		    const PathAnalysisPt *path_ap)
{
  ClkInfo *to_clk_info = from_clk_info;
  bool changed = false;
  ClockEdge *from_clk_edge = from_clk_info->clkEdge();
  const TransRiseFall *clk_tr = from_clk_edge->transition();

  bool from_clk_prop = from_clk_info->isPropagated();
  bool to_clk_prop = from_clk_prop;
  if (!from_clk_prop
      && sdc_->isPropagatedClock(to_pin)) {
    to_clk_prop = true;
    changed = true;
  }

  // Distinguish gen clk src path ClkInfo at generated clock roots,
  // so that generated clock crpr info can be (later) safely set on
  // the clkinfo.
  const Pin *gen_clk_src = NULL;
  if (from_clk_info->isGenClkSrcPath()
      && sdc_->crprActive()
      && sdc_->isClock(to_pin)) {
    // Don't care that it could be a regular clock root.
    gen_clk_src = to_pin;
    changed = true;
  }

  PathVertex *to_crpr_clk_path = NULL;
  if (sdc_->crprActive()
      && to_vertex->isRegClk()) {
    to_crpr_clk_path = from_path;
    changed = true;
  }

  // Propagate liberty "pulse_clock" transition to transitive fanout.
  TransRiseFall *from_pulse_sense = from_clk_info->pulseClkSense();
  TransRiseFall *to_pulse_sense = from_pulse_sense;
  LibertyPort *port = network_->libertyPort(to_pin);
  if (port && port->pulseClkSense()) {
    to_pulse_sense = port->pulseClkSense();
    changed = true;
  }
  else if (from_pulse_sense &&
	   edge->timingArcSet()->sense() == timing_sense_negative_unate) {
    to_pulse_sense = from_pulse_sense->opposite();
    changed = true;
  }

  Clock *from_clk = from_clk_info->clock();
  Arrival to_insertion = from_clk_info->insertion();
  float to_latency = from_clk_info->latency();
  float latency;
  bool exists;
  sdc_->clockLatency(from_clk, to_pin, clk_tr, min_max,
			     latency, exists);
  if (exists) {
    // Latency on pin has precidence over fanin or hierarchical
    // pin latency.
    to_latency = latency;
    to_clk_prop = false;
    changed = true;
  }
  else {
    // Check for hierarchical pin latency thru edge.
    sdc_->clockLatency(edge, clk_tr, min_max,
			       latency, exists);
    if (exists) {
      to_latency = latency;
      to_clk_prop = false;
      changed = true;
    }
  }

  ClockUncertainties *to_uncertainties = from_clk_info->uncertainties();
  ClockUncertainties *uncertainties = sdc_->clockUncertainties(to_pin);
  if (uncertainties) {
    to_uncertainties = uncertainties;
    changed = true;
  }

  if (changed)
    to_clk_info = findClkInfo(from_clk_edge, from_clk_info->clkSrc(),
			      to_clk_prop, gen_clk_src,
			      from_clk_info->isGenClkSrcPath(),
			      to_pulse_sense, to_insertion, to_latency,
			      to_uncertainties, path_ap, to_crpr_clk_path);
  return to_clk_info;
}

// Find the tag for a path going from from_tag thru edge to to_pin.
Tag *
Search::mutateTag(Tag *from_tag,
		  const Pin *from_pin,
		  const TransRiseFall *from_tr,
		  bool from_is_clk,
		  ClkInfo *from_clk_info,
		  const Pin *to_pin,
		  const TransRiseFall *to_tr,
		  bool to_is_clk,
		  bool to_is_reg_clk,
		  bool to_is_segment_start,
		  ClkInfo *to_clk_info,
		  InputDelay *to_input_delay,
		  const MinMax *min_max,
		  const PathAnalysisPt *path_ap)
{
  ExceptionStateSet *new_states = NULL;
  ExceptionStateSet *from_states = from_tag->states();
  if (from_states) {
    // Check for state changes in from_tag (but postpone copying state set).
    bool state_change = false;
    ExceptionStateSet::ConstIterator state_iter(from_states);
    while (state_iter.hasNext()) {
      ExceptionState *state = state_iter.next();
      ExceptionPath *exception = state->exception();
      if (state->isComplete()
	  && exception->isFalse()
	  && !from_is_clk)
	// Don't propagate a completed false path -thru unless it is a
	// clock (which ignores exceptions).
	return NULL;
      if (state->matchesNextThru(from_pin,to_pin,to_tr,min_max,network_)) {
	// Found a -thru that we've been waiting for.
	if (state->nextState()->isComplete()
	    && exception->isLoop())
	  // to_pin/edge completes a loop path.
	  return NULL;
	state_change = true;
	break;
      }
      // Kill loop tags at register clock pins.
      if (to_is_reg_clk && exception->isLoop()) {
	state_change = true;
	break;
      }
    }
    // Get the set of -thru exceptions starting at to_pin/edge.
    sdc_->exceptionThruStates(from_pin, to_pin, to_tr, min_max,
				      new_states);
    if (new_states || state_change) {
      // Second pass to apply state changes and add updated existing
      // states to new states.
      if (new_states == NULL)
	new_states = new ExceptionStateSet;
      ExceptionStateSet::ConstIterator state_iter(from_states);
      while (state_iter.hasNext()) {
	ExceptionState *state = state_iter.next();
	ExceptionPath *exception = state->exception();
	if (state->isComplete()
	    && exception->isFalse()
	    && !from_is_clk) {
	  // Don't propagate a completed false path -thru unless it is a
	  // clock. Clocks carry the completed false path to disable
	  // downstream paths that use the clock as data.
	  delete new_states;
	  return NULL;
	}
	// One edge may traverse multiple hierarchical thru pins.
	while (state->matchesNextThru(from_pin,to_pin,to_tr,min_max,network_))
	  // Found a -thru that we've been waiting for.
	  state = state->nextState();

	if (state->isComplete()
	    && exception->isLoop()) {
	  // to_pin/edge completes a loop path.
	  delete new_states;
	  return NULL;
	}

	// Kill loop tags at register clock pins.
	if (!(to_is_reg_clk
	      && exception->isLoop()))
	  new_states->insert(state);
      }
    }
  }
  else
    // Get the set of -thru exceptions starting at to_pin/edge.
    sdc_->exceptionThruStates(from_pin, to_pin, to_tr, min_max,
				      new_states);

  if (new_states)
    return findTag(to_tr, path_ap, to_clk_info, to_is_clk,
		   from_tag->inputDelay(), to_is_segment_start,
		   new_states, true);
  else {
    // No state change.
    if (to_clk_info == from_clk_info
	&& to_tr == from_tr
	&& to_is_clk == from_is_clk
	&& from_tag->isSegmentStart() == to_is_segment_start
	&& from_tag->inputDelay() == to_input_delay)
      return from_tag;
    else
      return findTag(to_tr, path_ap, to_clk_info, to_is_clk,
		     to_input_delay, to_is_segment_start,
		     from_states, false);
  }
}

TagGroup *
Search::findTagGroup(TagGroupBldr *tag_bldr)
{
  TagGroup probe(tag_bldr);
  TagGroup *tag_group = tag_group_set_->findKey(&probe);
  if (tag_group == NULL) {
    // Recheck with lock.
    tag_group_lock_.lock();
    tag_group = tag_group_set_->findKey(&probe);
    if (tag_group == NULL) {
      tag_group = tag_bldr->makeTagGroup(tag_group_count_, this);
      tag_groups_[tag_group_count_++] = tag_group;
      tag_group_set_->insert(tag_group);
      // If tag_groups_ needs to grow make the new array and copy the
      // contents into it before updating tags_groups_ so that other threads
      // can use Search::tagGroup(TagGroupIndex) without returning gubbish.
      // std::vector doesn't seem to follow this protocol so multi-thread
      // search fails occasionally if a vector is used for tag_groups_.
      if (tag_group_count_ == tag_group_capacity_) {
	TagGroupIndex new_capacity = nextMersenne(tag_group_capacity_);
	TagGroup **new_tag_groups = new TagGroup*[new_capacity];
	memcpy(new_tag_groups, tag_groups_,
	       tag_group_capacity_ * sizeof(TagGroup*));
	TagGroup **old_tag_groups = tag_groups_;
	tag_groups_ = new_tag_groups;
	tag_group_capacity_ = new_capacity;
	delete [] old_tag_groups;
	tag_group_set_->resize(new_capacity);
      }
      if (tag_group_count_ > tag_group_index_max)
	internalError("max tag group index exceeded");
    }
    tag_group_lock_.unlock();
  }
  return tag_group;
}

void
Search::setVertexArrivals(Vertex *vertex,
			  TagGroupBldr *tag_bldr)
{
  if (tag_bldr->empty())
    deletePaths(vertex);
  else {
    TagGroup *prev_tag_group = tagGroup(vertex);
    Arrival *prev_arrivals = vertex->arrivals();
    PathVertexRep *prev_paths = vertex->prevPaths();

    TagGroup *tag_group = findTagGroup(tag_bldr);
    int arrival_count = tag_group->arrivalCount();
    bool has_requireds = vertex->hasRequireds();
    // Reuse arrival array if it is the same size.
    if (prev_tag_group
	&& arrival_count == prev_tag_group->arrivalCount()
	&& (!has_requireds
	    // Requireds can only be reused if the tag group is unchanged.
	    || tag_group == prev_tag_group)) {
      if  (tag_bldr->hasClkTag() || tag_bldr->hasGenClkSrcTag()) {
	if (prev_paths == NULL)
	  prev_paths = new PathVertexRep[arrival_count];
      }
      else {
	// Prev paths not required, delete stale ones.
	delete [] prev_paths;
	prev_paths = NULL;
	vertex->setPrevPaths(NULL);
      }
      tag_bldr->copyArrivals(tag_group, prev_arrivals, prev_paths);
      vertex->setTagGroupIndex(tag_group->index());
    }
    else {
      delete [] prev_arrivals;
      delete [] prev_paths;

      Arrival *arrivals = new Arrival[arrival_count];
      prev_paths = NULL;
      if  (tag_bldr->hasClkTag() || tag_bldr->hasGenClkSrcTag())
	prev_paths = new PathVertexRep[arrival_count];
      tag_bldr->copyArrivals(tag_group, arrivals, prev_paths);

      vertex->setTagGroupIndex(tag_group->index());
      vertex->setArrivals(arrivals);
      vertex->setPrevPaths(prev_paths);

      have_paths_ = true;
      if (has_requireds) {
	requiredInvalid(vertex);
	vertex->setHasRequireds(false);
      }
    }
  }
}

void
Search::reportArrivals(Vertex *vertex) const
{
  report_->print("Vertex %s\n", vertex->name(sdc_network_));
  TagGroup *tag_group = tagGroup(vertex);
  Arrival *arrivals = vertex->arrivals();
  if (tag_group) {
    report_->print("Group %u\n", tag_group->index());
    ArrivalMap::Iterator arrival_iter(tag_group->arrivalMap());
    while (arrival_iter.hasNext()) {
      Tag *tag;
      int arrival_index;
      arrival_iter.next(tag, arrival_index);
      PathAnalysisPt *path_ap = tag->pathAnalysisPt(this);
      const TransRiseFall *tr = tag->transition();
      report_->print(" %d %s %s %s",
		     arrival_index,
		     tr->asString(),
		     path_ap->pathMinMax()->asString(),
		     delayAsString(arrivals[arrival_index], units_));
      if (vertex->hasRequireds()) {
	int req_index;
	bool exists;
	tag_group->requiredIndex(tag, req_index, exists);
	if (exists)
	  report_->print(" / %s", delayAsString(arrivals[req_index], units_));
      }
      report_->print(" %s", tag->asString(this));
      if (tag_group->hasClkTag()) {
	PathVertex tmp;
	PathVertex *prev = crpr_->clkPathPrev(vertex, arrival_index, tmp);
	report_->print(" clk_prev=[%s]",
		       prev && !prev->isNull() ? prev->name(this) : "NULL");
      }
      report_->print("\n");
    }
  }
  else
    report_->print(" no arrivals\n");
}

TagGroup *
Search::tagGroup(TagGroupIndex index) const
{
  return tag_groups_[index];
}

TagGroup *
Search::tagGroup(const Vertex *vertex) const
{
  TagGroupIndex index = vertex->tagGroupIndex();
  if (index == tag_group_index_max)
    return NULL;
  else
    return tag_groups_[index];
}

TagGroupIndex
Search::tagGroupCount() const
{
  return tag_group_count_;
}

void
Search::reportTagGroups() const
{
  for (TagGroupIndex i = 0; i < tag_group_count_; i++) {
    TagGroup *tag_group = tag_groups_[i];
    if (tag_group) {
      report_->print("Group %4u hash = %4u (%4u)\n",
		     i,
		     tag_group->hash(),
		     tag_group->hash() % tag_group_set_->capacity());
      tag_group->reportArrivalMap(this);
    }
  }
  Hash long_hash = tag_group_set_->longestBucketHash();
  report_->print("Longest hash bucket length %lu hash=%lu\n",
		 tag_group_set_->bucketLength(long_hash),
		 long_hash);
}

void
Search::reportArrivalCountHistogram() const
{
  Vector<int> vertex_counts(10);
  VertexIterator vertex_iter(graph_);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    TagGroup *tag_group = tagGroup(vertex);
    if (tag_group) {
      int arrival_count = tag_group->arrivalCount();
      if (arrival_count >= static_cast<int>(vertex_counts.size()))
	vertex_counts.resize(arrival_count * 2);
      vertex_counts[arrival_count]++;
    }
  }

  for (int arrival_count = 0;
       arrival_count < static_cast<int>(vertex_counts.size());
       arrival_count++) {
    int vertex_count = vertex_counts[arrival_count];
    if (vertex_count > 0)
      report_->print("%6d %6d\n", arrival_count, vertex_count);
  }
}

////////////////////////////////////////////////////////////////

Tag *
Search::tag(TagIndex index) const
{
  Tag *tag = tags_[index];
  return tag;
}

TagIndex
Search::tagCount() const
{
  return tag_count_;
}

Tag *
Search::findTag(const TransRiseFall *tr,
		const PathAnalysisPt *path_ap,
		ClkInfo *clk_info,
		bool is_clk,
		InputDelay *input_delay,
		bool is_segment_start,
		ExceptionStateSet *states,
		bool own_states) const
{
  Tag probe(0, tr->index(), path_ap->index(), clk_info, is_clk, input_delay,
	    is_segment_start, states, false, this);
  Tag *tag = tag_set_->findKey(&probe);
  if (tag == NULL) {
    // Recheck with lock.
    tag_lock_.lock();
    tag = tag_set_->findKey(&probe);
    if (tag == NULL) {
      ExceptionStateSet *new_states = !own_states && states
	? new ExceptionStateSet(*states) : states;
      tag = new Tag(tag_count_, tr->index(), path_ap->index(),
		    clk_info, is_clk, input_delay, is_segment_start,
		    new_states, true, this);
      own_states = false;
      // Make sure tag can be indexed in tags_ before it is visible to
      // other threads via tag_set_.
      tags_[tag_count_++] = tag;
      tag_set_->insert(tag);
      // If tags_ needs to grow make the new array and copy the
      // contents into it before updating tags_ so that other threads
      // can use Search::tag(TagIndex) without returning gubbish.
      // std::vector doesn't seem to follow this protocol so multi-thread
      // search fails occasionally if a vector is used for tags_.
      if (tag_count_ == tag_capacity_) {
	TagIndex new_capacity = nextMersenne(tag_capacity_);
	Tag **new_tags = new Tag*[new_capacity];
	memcpy(new_tags, tags_, tag_count_ * sizeof(Tag*));
	Tag **old_tags = tags_;
	tags_ = new_tags;
	delete [] old_tags;
	tag_capacity_ = new_capacity;
	tag_set_->resize(new_capacity);
      }
      if (tag_count_ > tag_index_max)
	internalError("max tag index exceeded");
    }
    tag_lock_.unlock();
  }
  if (own_states)
    delete states;
  return tag;
}

void
Search::reportTags() const
{
  for (TagIndex i = 0; i < tag_count_; i++) {
    Tag *tag = tags_[i];
    report_->print("Tag %4u %4u %s\n",
		   tag->index(),
		   tag->hash() % tag_set_->capacity(),
		   tag->asString(false, this));
  }
  Hash long_hash = tag_set_->longestBucketHash();
  printf("Longest hash bucket length %d hash=%u\n",
	 tag_set_->bucketLength(long_hash),
	 long_hash);
}

void
Search::reportClkInfos() const
{
  Vector<ClkInfo*> clk_infos;
  ClkInfoSet::Iterator clk_info_iter1(clk_info_set_);
  while (clk_info_iter1.hasNext()) {
    ClkInfo *clk_info = clk_info_iter1.next();
    clk_infos.push_back(clk_info);
  }
  sort(clk_infos, ClkInfoLess(this));
  Vector<ClkInfo*>::Iterator clk_info_iter2(clk_infos);
  while (clk_info_iter2.hasNext()) {
    ClkInfo *clk_info = clk_info_iter2.next();
    report_->print("ClkInfo %s\n",
		   clk_info->asString(this));
  }
  printf("%lu clk infos\n",
	 clk_info_set_->size());
}

ClkInfo *
Search::findClkInfo(ClockEdge *clk_edge,
		    const Pin *clk_src,
		    bool is_propagated,
                    const Pin *gen_clk_src,
		    bool gen_clk_src_path,
		    const TransRiseFall *pulse_clk_sense,
		    Arrival insertion,
		    float latency,
		    ClockUncertainties *uncertainties,
                    const PathAnalysisPt *path_ap,
		    PathVertex *crpr_clk_path)
{
  PathVertexRep crpr_clk_path_rep(crpr_clk_path, this);
  ClkInfo probe(clk_edge, clk_src, is_propagated, gen_clk_src, gen_clk_src_path,
		pulse_clk_sense, insertion, latency, uncertainties,
		path_ap->index(), crpr_clk_path_rep, this);
  clk_info_lock_.lock();
  ClkInfo *clk_info = clk_info_set_->findKey(&probe);
  if (clk_info == NULL) {
    clk_info = new ClkInfo(clk_edge, clk_src,
			   is_propagated, gen_clk_src, gen_clk_src_path,
			   pulse_clk_sense, insertion, latency, uncertainties,
			   path_ap->index(), crpr_clk_path_rep, this);
    clk_info_set_->insert(clk_info);
  }
  clk_info_lock_.unlock();
  return clk_info;
}

ClkInfo *
Search::findClkInfo(ClockEdge *clk_edge,
		    const Pin *clk_src,
		    bool is_propagated,
		    Arrival insertion,
		    const PathAnalysisPt *path_ap)
{
  return findClkInfo(clk_edge, clk_src, is_propagated, NULL, false, NULL,
		     insertion, 0.0, NULL, path_ap, NULL);
}

int
Search::clkInfoCount() const
{
  return clk_info_set_->size();
}

ArcDelay
Search::deratedDelay(Vertex *from_vertex,
		     TimingArc *arc,
		     Edge *edge,
		     bool is_clk,
		     const PathAnalysisPt *path_ap)
{
  const DcalcAnalysisPt *dcalc_ap = path_ap->dcalcAnalysisPt();
  DcalcAPIndex ap_index = dcalc_ap->index();
  float derate = timingDerate(from_vertex, arc, edge, is_clk, path_ap);
  ArcDelay delay = graph_->arcDelay(edge, arc, ap_index);
  return delay + Delay((derate - 1.0) * delayAsFloat(delay));
}

float
Search::timingDerate(Vertex *from_vertex,
		     TimingArc *arc,
		     Edge *edge,
		     bool is_clk,
		     const PathAnalysisPt *path_ap)
{
  PathClkOrData derate_clk_data = is_clk ? path_clk : path_data;
  TimingRole *role = edge->role();
  const Pin *pin = from_vertex->pin();
  if (role->isWire()) {
    const TransRiseFall *tr = arc->toTrans()->asRiseFall();
    return sdc_->timingDerateNet(pin, derate_clk_data,
					 tr, path_ap->pathMinMax());
  }
  else {
    TimingDerateType derate_type;
    const TransRiseFall *tr;
    if (role->isTimingCheck()) {
      derate_type = timing_derate_cell_check;
      tr = arc->toTrans()->asRiseFall();
    }
    else {
       derate_type = timing_derate_cell_delay;
       tr = arc->fromTrans()->asRiseFall();
    }
    return sdc_->timingDerateInstance(pin, derate_type,
					      derate_clk_data, tr,
					      path_ap->pathMinMax());
  }
}

void
Search::clocks(const Vertex *vertex,
	       // Return value.
	       ClockSet &clks) const
{
  VertexPathIterator path_iter(const_cast<Vertex*>(vertex), this);
  while (path_iter.hasNext()) {
    Path *path = path_iter.next();
    if (path->isClock(this))
      clks.insert(path->clock(this));
  }
}

bool
Search::isClock(const Vertex *vertex) const
{
  TagGroup *tag_group = tagGroup(vertex);
  if (tag_group)
    return tag_group->hasClkTag();
  else
    return false;
}

bool
Search::isGenClkSrc(const Vertex *vertex) const
{
  TagGroup *tag_group = tagGroup(vertex);
  if (tag_group)
    return tag_group->hasGenClkSrcTag();
  else
    return false;
}

void
Search::clocks(const Pin *pin,
	       // Return value.
	       ClockSet &clks) const
{
  Vertex *vertex;
  Vertex *bidirect_drvr_vertex;
  graph_->pinVertices(pin, vertex, bidirect_drvr_vertex);
  if (vertex)
    clocks(vertex, clks);
  if (bidirect_drvr_vertex)
    clocks(bidirect_drvr_vertex, clks);
}

////////////////////////////////////////////////////////////////

void
Search::findRequireds()
{
  Stats stats(debug_);
  findRequireds(0);
  stats.report("Find requireds");
}

void
Search::findRequireds(Level level)
{
  debugPrint1(debug_, "search", 1, "find requireds to level %d\n", level);
  RequiredVisitor req_visitor(this);
  if (!requireds_seeded_)
    seedRequireds();
  seedInvalidRequireds();
  int required_count = required_iter_->visitParallel(level, &req_visitor);
  requireds_exist_ = true;
  debugPrint1(debug_, "search", 1, "found %d requireds\n", required_count);
}

void
Search::seedRequireds()
{
  ensureDownstreamClkPins();
  VertexSet::Iterator end_iter(endpoints());
  while (end_iter.hasNext()) {
    Vertex *vertex = end_iter.next();
    seedRequired(vertex);
  }
  requireds_seeded_ = true;
  requireds_exist_ = true;
}

VertexSet *
Search::endpoints()
{
  if (endpoints_ == NULL) {
    endpoints_ = new VertexSet;
    invalid_endpoints_ = new VertexSet;
    VertexIterator vertex_iter(graph_);
    while (vertex_iter.hasNext()) {
      Vertex *vertex = vertex_iter.next();
      if (isEndpoint(vertex)) {
	debugPrint1(debug_, "endpoint", 2, "insert %s\n",
		    vertex->name(sdc_network_));
	endpoints_->insert(vertex);
      }
    }
  }
  if (invalid_endpoints_) {
    VertexSet::Iterator vertex_iter(invalid_endpoints_);
    while (vertex_iter.hasNext()) {
      Vertex *vertex = vertex_iter.next();
      if (isEndpoint(vertex)) {
	debugPrint1(debug_, "endpoint", 2, "insert %s\n",
		    vertex->name(sdc_network_));
	endpoints_->insert(vertex);
      }
      else {
	if (debug_->check("endpoint", 2)
	    && endpoints_->hasKey(vertex))
	  debug_->print("endpoint: remove %s\n", vertex->name(sdc_network_));
	endpoints_->eraseKey(vertex);
      }
    }
    invalid_endpoints_->clear();
  }
  return endpoints_;
}

void
Search::endpointInvalid(Vertex *vertex)
{
  if (invalid_endpoints_) {
    debugPrint1(debug_, "endpoint", 2, "invalid %s\n",
		vertex->name(sdc_network_));
    invalid_endpoints_->insert(vertex);
  }
}

bool
Search::isEndpoint(Vertex *vertex) const
{
  return isEndpoint(vertex, search_adj_);
}

bool
Search::isEndpoint(Vertex *vertex,
		   SearchPred *pred) const
{
  Pin *pin = vertex->pin();
  return hasFanin(vertex, pred, graph_)
    && ((vertex->hasChecks()
	 && hasEnabledChecks(vertex))
	|| (sdc_->gatedClkChecksEnabled()
	    && gated_clk_->isGatedClkEnable(vertex))
	|| vertex->isConstrained()
	|| sdc_->isPathDelayInternalEndpoint(pin)
	|| !hasFanout(vertex, pred, graph_)
	// Unconstrained paths at register clk pins.
	|| (report_unconstrained_paths_
	    && vertex->isRegClk()));
}

bool
Search::hasEnabledChecks(Vertex *vertex) const
{
  VertexInEdgeIterator edge_iter(vertex, graph_);
  while (edge_iter.hasNext()) {
    Edge *edge = edge_iter.next();
    if (visit_path_ends_->checkEdgeEnabled(edge))
      return true;
  }
  return false;
}

void
Search::endpointsInvalid()
{
  delete endpoints_;
  delete invalid_endpoints_;
  endpoints_ = NULL;
  invalid_endpoints_ = NULL;
}

void
Search::seedInvalidRequireds()
{
  VertexSet::Iterator vertex_iter(invalid_requireds_);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    required_iter_->enqueue(vertex);
  }
  invalid_requireds_.clear();
}

////////////////////////////////////////////////////////////////

// Visitor used by visitPathEnds to seed end required time.
class FindEndRequiredVisitor : public PathEndVisitor
{
public:
  FindEndRequiredVisitor(RequiredCmp *required_cmp,
			 const StaState *sta);
  FindEndRequiredVisitor(const StaState *sta);
  virtual ~FindEndRequiredVisitor();
  virtual PathEndVisitor *copy();
  virtual void visit(PathEnd *path_end);

protected:
  const StaState *sta_;
  RequiredCmp *required_cmp_;
  bool own_required_cmp_;
};

FindEndRequiredVisitor::FindEndRequiredVisitor(RequiredCmp *required_cmp,
					       const StaState *sta) :
  sta_(sta),
  required_cmp_(required_cmp),
  own_required_cmp_(false)
{
}

FindEndRequiredVisitor::FindEndRequiredVisitor(const StaState *sta) :
  sta_(sta),
  required_cmp_(new RequiredCmp),
  own_required_cmp_(true)
{
}

FindEndRequiredVisitor::~FindEndRequiredVisitor()
{
  if (own_required_cmp_)
    delete required_cmp_;
}

PathEndVisitor *
FindEndRequiredVisitor::copy()
{
  return new FindEndRequiredVisitor(sta_);
}

void
FindEndRequiredVisitor::visit(PathEnd *path_end)
{
  if (!path_end->isUnconstrained()) {
    PathRef &path = path_end->pathRef();
    const MinMax *req_min = path.minMax(sta_)->opposite();
    int arrival_index;
    bool arrival_exists;
    path.arrivalIndex(arrival_index, arrival_exists);
    Required required = path_end->requiredTime(sta_);
    required_cmp_->requiredSet(arrival_index, required, req_min);
  }
}

void
Search::seedRequired(Vertex *vertex)
{
  debugPrint1(debug_, "search", 2, "required seed %s\n",
	      vertex->name(sdc_network_));
  RequiredCmp required_cmp;
  FindEndRequiredVisitor seeder(&required_cmp, this);
  required_cmp.requiredsInit(vertex, this);
  visit_path_ends_->visitPathEnds(vertex, &seeder);
  // Enqueue fanin vertices for back-propagating required times.
  if (required_cmp.requiredsSave(vertex, this))
    required_iter_->enqueueAdjacentVertices(vertex);
}

void
Search::seedRequiredEnqueueFanin(Vertex *vertex)
{
  RequiredCmp required_cmp;
  FindEndRequiredVisitor seeder(&required_cmp, this);
  required_cmp.requiredsInit(vertex, this);
  visit_path_ends_->visitPathEnds(vertex, &seeder);
  // Enqueue fanin vertices for back-propagating required times.
  required_cmp.requiredsSave(vertex, this);
  required_iter_->enqueueAdjacentVertices(vertex);
}

////////////////////////////////////////////////////////////////

RequiredCmp::RequiredCmp() :
  have_requireds_(false)
{
  requireds_.reserve(10);
}

void
RequiredCmp::requiredsInit(Vertex *vertex,
			   const StaState *sta)
{
  Search *search = sta->search();
  TagGroup *tag_group = search->tagGroup(vertex);
  if (tag_group) {
    requireds_.resize(tag_group->arrivalCount());
    ArrivalMap *arrival_entries = tag_group->arrivalMap();
    ArrivalMap::Iterator arrival_iter(arrival_entries);
    while (arrival_iter.hasNext()) {
      Tag *tag;
      int arrival_index;
      arrival_iter.next(tag, arrival_index);
      PathAnalysisPt *path_ap = tag->pathAnalysisPt(sta);
      const MinMax *min_max = path_ap->pathMinMax();
      requireds_[arrival_index] = delayInitValue(min_max->opposite());
    }
  }
  else
    requireds_.resize(0);
  have_requireds_ = false;
}

void
RequiredCmp::requiredSet(int arrival_index,
			 Required required,
			 const MinMax *min_max)
{
  if (delayFuzzyGreater(required, requireds_[arrival_index], min_max)) {
    requireds_[arrival_index] = required;
    have_requireds_ = true;
  }
}

bool
RequiredCmp::requiredsSave(Vertex *vertex,
			   const StaState *sta)
{
  bool requireds_changed = false;
  bool prev_reqs = vertex->hasRequireds();
  if (have_requireds_) {
    if (!prev_reqs)
      requireds_changed = true;
    Debug *debug = sta->debug();
    VertexPathIterator path_iter(vertex, sta);
    while (path_iter.hasNext()) {
      PathVertex *path = path_iter.next();
      int arrival_index;
      bool arrival_exists;
      path->arrivalIndex(arrival_index, arrival_exists);
      Required req = requireds_[arrival_index];
      if (prev_reqs) {
	Required prev_req = path->required(sta);
	if (!delayFuzzyEqual(prev_req, req)) {
	  debugPrint2(debug, "search", 3, "required save %s -> %s\n",
		      delayAsString(prev_req, sta->units()),
		      delayAsString(req, sta->units()));
	  path->setRequired(req, sta);
	  requireds_changed = true;
	}
      }
      else {
	debugPrint1(debug, "search", 3, "required save MIA -> %s\n",
		    delayAsString(req, sta->units()));
	path->setRequired(req, sta);
      }
    }
  }
  else if (prev_reqs) {
    PathVertex::deleteRequireds(vertex, sta);
    requireds_changed = true;
  }
  return requireds_changed;
}

Required
RequiredCmp::required(int arrival_index)
{
  return requireds_[arrival_index];
}

////////////////////////////////////////////////////////////////

RequiredVisitor::RequiredVisitor(const StaState *sta) :
  PathVisitor(sta),
  required_cmp_(new RequiredCmp),
  visit_path_ends_(new VisitPathEnds(sta))
{
}

RequiredVisitor::~RequiredVisitor()
{
  delete required_cmp_;
  delete visit_path_ends_;
}

VertexVisitor *
RequiredVisitor::copy()
{
  return new RequiredVisitor(sta_);
}

void
RequiredVisitor::visit(Vertex *vertex)
{
  Search *search = sta_->search();
  const Debug *debug = sta_->debug();
  debugPrint1(debug, "search", 2, "find required %s\n",
	      vertex->name(sta_->network()));
  required_cmp_->requiredsInit(vertex, sta_);
  // Back propagate requireds from fanout.
  visitFanoutPaths(vertex);
  // Check for constraints at endpoints that set required times.
  if (search->isEndpoint(vertex)) {
    FindEndRequiredVisitor seeder(required_cmp_, sta_);
    visit_path_ends_->visitPathEnds(vertex, &seeder);
  }
  bool changed = required_cmp_->requiredsSave(vertex, sta_);
  search->tnsInvalid(vertex);

  if (changed)
    search->requiredIterator()->enqueueAdjacentVertices(vertex);
}

bool
RequiredVisitor::visitFromToPath(const Pin *,
				 Vertex *,
				 const TransRiseFall *from_tr,
				 Tag *from_tag,
				 PathVertex *from_path,
				 Edge *edge,
				 TimingArc *,
				 ArcDelay arc_delay,
				 Vertex *to_vertex,
				 const TransRiseFall *to_tr,
				 Tag *to_tag,
				 Arrival &,
				 const MinMax *min_max,
				 const PathAnalysisPt *path_ap)
{
  // Don't propagate required times through latch D->Q edges.
  if (edge->role() != TimingRole::latchDtoQ()) {
    const Debug *debug = sta_->debug();
    debugPrint3(debug, "search", 3, "  %s -> %s %s\n",
		from_tr->asString(),
		to_tr->asString(),
		min_max->asString());
    debugPrint2(debug, "search", 3, "  from tag %2u: %s\n",
		from_tag->index(),
		from_tag->asString(sta_));
    int arrival_index;
    bool arrival_exists;
    from_path->arrivalIndex(arrival_index, arrival_exists);
    const MinMax *req_min = min_max->opposite();
    TagGroup *to_tag_group = sta_->search()->tagGroup(to_vertex);
    // Check to see if to_tag was pruned.
    if (to_tag_group->hasTag(to_tag)) {
      PathVertex to_path(to_vertex, to_tag, sta_);
      Required to_required = to_path.required(sta_);
      Required from_required = to_required - arc_delay;
      debugPrint2(debug, "search", 3, "  to tag   %2u: %s\n",
		  to_tag->index(),
		  to_tag->asString(sta_));
      debugPrint5(debug, "search", 3, "  %s - %s = %s %s %s\n",
		  delayAsString(to_required, sta_),
		  delayAsString(arc_delay, sta_),
		  delayAsString(from_required, sta_),
		  min_max == MinMax::max() ? "<" : ">",
		  delayAsString(required_cmp_->required(arrival_index), sta_));
      required_cmp_->requiredSet(arrival_index, from_required, req_min);
    }
    else {
      // Arrival that differ by crpr_pin may be pruned. Find an arrival
      // that matches everything but the crpr_pin.
      VertexPathIterator to_iter(to_vertex, to_tr, path_ap, sta_);
      while (to_iter.hasNext()) {
	PathVertex *to_path = to_iter.next();
	if (tagMatchNoCrpr(to_path->tag(sta_), to_tag)) {
	  Required to_required = to_path->required(sta_);
	  Required from_required = to_required - arc_delay;
	  debugPrint2(debug, "search", 3, "  to tag   %2u: %s\n",
		      to_tag->index(),
		      to_tag->asString(sta_));
	  debugPrint5(debug, "search", 3, "  %s - %s = %s %s %s\n",
		      delayAsString(to_required, sta_),
		      delayAsString(arc_delay, sta_),
		      delayAsString(from_required, sta_),
		      min_max == MinMax::max() ? "<" : ">",
		      delayAsString(required_cmp_->required(arrival_index),
				    sta_));
	  required_cmp_->requiredSet(arrival_index, from_required, req_min);
	  break;
	}
      }
    }
  }
  return true;
}

////////////////////////////////////////////////////////////////

void
Search::ensureDownstreamClkPins()
{
  if (!found_downstream_clk_pins_) {
    // Use backward BFS from register clk pins to mark upsteam pins
    // as having downstream clk pins.
    ClkTreeSearchPred pred(this);
    BfsBkwdIterator iter(bfs_other, &pred, this);
    VertexSet::ConstIterator reg_clk_iter(graph_->regClkVertices());
    while (reg_clk_iter.hasNext()) {
      Vertex *vertex = reg_clk_iter.next();
      iter.enqueue(vertex);
    }
    // Enqueue PLL feedback pins.
    VertexIterator vertex_iter(graph_);
    while (vertex_iter.hasNext()) {
      Vertex *vertex = vertex_iter.next();
      Pin *pin = vertex->pin();
      const LibertyPort *port = network_->libertyPort(pin);
      if (port && port->isPllFeedbackPin())
	iter.enqueue(vertex);
    }
    while (iter.hasNext()) {
      Vertex *vertex = iter.next();
      vertex->setHasDownstreamClkPin(true);
      iter.enqueueAdjacentVertices(vertex);
    }
  }
  found_downstream_clk_pins_ = true;
}

////////////////////////////////////////////////////////////////

bool
Search::matchesFilter(Path *path,
		      const ClockEdge *to_clk_edge)
{
  if (filter_ == NULL
      && filter_from_ == NULL
      && filter_to_ == NULL)
    return true;
  else if (filter_) {
    // -from pins|inst
    // -thru
    // Path has to be tagged by traversing the filter exception points.
    ExceptionStateSet *states = path->tag(this)->states();
    if (states) {
      ExceptionStateSet::ConstIterator state_iter(states);
      while (state_iter.hasNext()) {
	ExceptionState *state = state_iter.next();
	if (state->exception() == filter_
	    && state->nextThru() == NULL
	    && matchesFilterTo(path, to_clk_edge))
	  return true;
      }
    }
    return false;
  }
  else if (filter_from_
	   && filter_from_->pins() == NULL
	   && filter_from_->instances() == NULL
	   && filter_from_->clks()) {
    // -from clks
    ClockEdge *path_clk_edge = path->clkEdge(this);
    Clock *path_clk = path_clk_edge ? path_clk_edge->clock() : NULL;
    TransRiseFall *path_clk_tr =
      path_clk_edge ? path_clk_edge->transition() : NULL;
    return filter_from_->clks()->hasKey(path_clk)
      && filter_from_->transition()->matches(path_clk_tr)
      && matchesFilterTo(path, to_clk_edge);
  }
  else if (filter_from_ == NULL
	   && filter_to_)
    // -to
    return matchesFilterTo(path, to_clk_edge);
  else
    internalError("unexpected filter path");
}

// Similar to Constraints::exceptionMatchesTo.
bool
Search::matchesFilterTo(Path *path,
			const ClockEdge *to_clk_edge) const
{
  return (filter_to_ == NULL
	  || filter_to_->matchesFilter(path->pin(graph_), to_clk_edge,
				       path->transition(this), network_));
}

////////////////////////////////////////////////////////////////

// Find the exception that has the highest priority for an end path,
// including exceptions that start at the end pin or target clock.
ExceptionPath *
Search::exceptionTo(ExceptionPathType type,
		    const Path *path,
		    const Pin *pin,
		    const TransRiseFall *tr,
		    const ClockEdge *clk_edge,
		    const MinMax *min_max,
		    bool match_min_max_exactly,
		    bool require_to_pin) const
{
  // Find the highest priority exception carried by the path's tag.
  int hi_priority = -1;
  ExceptionPath *hi_priority_exception = NULL;
  const ExceptionStateSet *states = path->tag(this)->states();
  if (states) {
    ExceptionStateSet::ConstIterator state_iter(states);
    while (state_iter.hasNext()) {
      ExceptionState *state = state_iter.next();
      ExceptionPath *exception = state->exception();
      int priority = exception->priority(min_max);
      if ((type == exception_type_any
	   || exception->type() == type)
	  && sdc_->isCompleteTo(state, pin, tr, clk_edge, min_max,
					match_min_max_exactly, require_to_pin)
	  && (hi_priority_exception == NULL
	      || priority > hi_priority
	      || (priority == hi_priority
		  && exception->tighterThan(hi_priority_exception)))) {
	hi_priority = priority;
	hi_priority_exception = exception;
      }
    }
  }
  // Check for -to exceptions originating at the end pin or target clock.
  sdc_->exceptionTo(type, pin, tr, clk_edge, min_max,
		    match_min_max_exactly,
		    hi_priority_exception, hi_priority);
  return hi_priority_exception;
}

////////////////////////////////////////////////////////////////

Slack
Search::totalNegativeSlack(const MinMax *min_max)
{
  wnsTnsPreamble();
  if (tns_exists_)
    updateInvalidTns();
  else
    findTotalNegativeSlacks();
  return static_cast<Slack>(tns_[min_max->index()]);
}

void
Search::tnsInvalid(Vertex *vertex)
{
  if ((tns_exists_ || worst_slacks_)
      && isEndpoint(vertex)) {
    debugPrint1(debug_, "tns", 2, "tns invalid %s\n",
		vertex->name(sdc_network_));
    tns_lock_.lock();
    invalid_tns_.insert(vertex);
    tns_lock_.unlock();
  }
}

void
Search::updateInvalidTns()
{
  VertexSet::Iterator vertex_iter(invalid_tns_);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    // Network edits can change endpointedness since tnsInvalid was called.
    if (isEndpoint(vertex)) {
      debugPrint1(debug_, "tns", 2, "update tns %s\n",
		  vertex->name(sdc_network_));
      Slack slacks[MinMax::index_count];
      wnsSlacks(vertex, slacks);

      if (tns_exists_)
	updateTns(vertex, slacks);
      if (worst_slacks_)
	worst_slacks_->updateWorstSlacks(vertex, slacks);
    }
  }
  invalid_tns_.clear();
}

void
Search::findTotalNegativeSlacks()
{
  int min_index = MinMax::minIndex();
  int max_index = MinMax::maxIndex();
  tns_[min_index] = 0.0;
  tns_[max_index] = 0.0;
  tns_slacks_[min_index].clear();
  tns_slacks_[max_index].clear();
  VertexSet::Iterator end_iter(endpoints());
  while (end_iter.hasNext()) {
    Vertex *vertex = end_iter.next();
    // No locking required.
    Slack slacks[MinMax::index_count];
    wnsSlacks(vertex, slacks);
    tnsIncr(vertex, delayAsFloat(slacks[min_index]), min_index);
    tnsIncr(vertex, delayAsFloat(slacks[max_index]), max_index);
  }
  tns_exists_ = true;
}

void
Search::updateTns(Vertex *vertex,
		  Slack *slacks)
{
  int min_index = MinMax::minIndex();
  int max_index = MinMax::maxIndex();
  tnsDecr(vertex, min_index);
  tnsIncr(vertex, delayAsFloat(slacks[min_index]), min_index);

  tnsDecr(vertex, max_index);
  tnsIncr(vertex, delayAsFloat(slacks[max_index]), max_index);
}

void
Search::tnsIncr(Vertex *vertex,
		float slack,
		int min_max_index)
{
  if (fuzzyLess(slack, 0.0)) {
    debugPrint2(debug_, "tns", 3, "tns+ %s %s\n",
		delayAsString(slack, units_),
		vertex->name(sdc_network_));
    tns_[min_max_index] += slack;
    if (tns_slacks_[min_max_index].hasKey(vertex))
      internalError("tns incr existing vertex");
    tns_slacks_[min_max_index][vertex] = slack;
  }
}

void
Search::tnsDecr(Vertex *vertex,
		int min_max_index)
{
  Slack slack;
  bool found;
  tns_slacks_[min_max_index].findKey(vertex, slack, found);
  if (found
      && delayFuzzyLess(slack, 0.0)) {
    debugPrint2(debug_, "tns", 3, "tns- %s %s\n",
		delayAsString(slack, units_),
		vertex->name(sdc_network_));
    tns_[min_max_index] -= delayAsFloat(slack);
    tns_slacks_[min_max_index].eraseKey(vertex);
  }
}

// Notify tns before updating/deleting slack (arrival/required).
void
Search::tnsNotifyBefore(Vertex *vertex)
{
  if (tns_exists_
      && isEndpoint(vertex)) {
    int min_index = MinMax::minIndex();
    int max_index = MinMax::maxIndex();
    tnsDecr(vertex, min_index);
    tnsDecr(vertex, max_index);
  }
}

////////////////////////////////////////////////////////////////

Slack
Search::worstSlack(const MinMax *min_max)
{
  worstSlackPreamble();
  return worst_slacks_->worstSlack(min_max);
}

Vertex *
Search::worstSlackVertex(const MinMax *min_max)
{
  worstSlackPreamble();
  return worst_slacks_->worstSlackVertex(min_max);
}

void
Search::wnsTnsPreamble()
{
  findAllArrivals();
  // Required times are only needed at endpoints.
  if (requireds_seeded_) {
    VertexSet::Iterator vertex_iter(invalid_requireds_);
    while (vertex_iter.hasNext()) {
      Vertex *vertex = vertex_iter.next();
      debugPrint1(debug_, "search", 2, "tns update required %s\n",
		  vertex->name(sdc_network_));
      if (isEndpoint(vertex)) {
	seedRequired(vertex);
	// If the endpoint has fanout it's required time
	// depends on downstream checks, so enqueue it to
	// force required propagation to it's level if
	// the required time is requested later.
	if (hasFanout(vertex, search_adj_, graph_))
	  required_iter_->enqueue(vertex);
      }
      invalid_requireds_.eraseKey(vertex);
    }
  }
  else
    seedRequireds();
}

void
Search::worstSlackPreamble()
{
  wnsTnsPreamble();
  if (worst_slacks_)
    updateInvalidTns();
  else
    worst_slacks_ = new WorstSlacks(this);
}

void
Search::clearWorstSlack()
{
  if (worst_slacks_) {
    // Don't maintain incremental worst slacks until there is a request.
    delete worst_slacks_;
    worst_slacks_ = NULL;
  }
}

////////////////////////////////////////////////////////////////

class FindEndSlackVisitor : public PathEndVisitor
{
public:
  FindEndSlackVisitor(Slack *slacks,
		      const StaState *sta);
  virtual PathEndVisitor *copy();
  virtual void visit(PathEnd *path_end);

protected:
  Slack *slacks_;
  const StaState *sta_;
};

FindEndSlackVisitor::FindEndSlackVisitor(Slack *slacks,
					 const StaState *sta) :
  slacks_(slacks),
  sta_(sta)
{
}

PathEndVisitor *
FindEndSlackVisitor::copy()
{

  return new FindEndSlackVisitor(slacks_, sta_);
}

void
FindEndSlackVisitor::visit(PathEnd *path_end)
{
  if (!path_end->isUnconstrained()) {
    PathRef &path = path_end->pathRef();
    int mm_index = path.minMax(sta_)->index();
    Slack slack = path_end->slack(sta_);
    if (delayFuzzyLess(slack, slacks_[mm_index]))
      slacks_[mm_index] = slack;
  }
}

void
Search::wnsSlacks(Vertex *vertex,
		  // Return values.
		  Slack *slacks)
{
  Slack slack_init = MinMax::min()->initValue();
  slacks[MinMax::minIndex()] = slack_init;
  slacks[MinMax::maxIndex()] = slack_init;
  if (hasFanout(vertex, search_adj_, graph_)) {
    // If the vertex has fanout the path slacks include downstream
    // PathEnd slacks so find the endpoint slack directly.
    FindEndSlackVisitor end_visitor(slacks, this);
    visit_path_ends_->visitPathEnds(vertex, &end_visitor);
  }
  else {
    VertexPathIterator path_iter(vertex, this);
    while (path_iter.hasNext()) {
      Path *path = path_iter.next();
      const MinMax *path_min_max = path->minMax(this);
      int path_mm_index = path_min_max->index();
      const Slack path_slack = path->slack(this);
      if (!path->tag(this)->isFilter()
	  && delayFuzzyLess(path_slack, slacks[path_mm_index]))
	slacks[path_mm_index] = path_slack;
    }
  }
}

Slack
Search::wnsSlack(Vertex *vertex,
		 const MinMax *min_max)
{
  Slack slacks[MinMax::index_count];
  wnsSlacks(vertex, slacks);
  return slacks[min_max->index()];
}

////////////////////////////////////////////////////////////////

PathGroups *
Search::makePathGroups(int max_paths,
		       int nworst,
		       bool unique_pins,
		       float slack_min,
		       float slack_max,
		       PathGroupNameSet *group_names,
		       bool setup,
		       bool hold,
		       bool recovery,
		       bool removal,
		       bool clk_gating_setup,
		       bool clk_gating_hold)
{
  return new PathGroups(max_paths, nworst, unique_pins,
			slack_min, slack_max,
			group_names,
			setup, hold,
			recovery, removal,
			clk_gating_setup, clk_gating_hold,
			report_unconstrained_paths_,
			this);
}

void
Search::deletePathGroups()
{
  delete path_groups_;
  path_groups_ = NULL;
}

PathGroup *
Search::pathGroup(const PathEnd *path_end) const
{
  return path_groups_->pathGroup(path_end);
}

bool
Search::havePathGroups() const
{
  return path_groups_ != NULL;
}

PathGroup *
Search::findPathGroup(const char *name,
		      const MinMax *min_max) const
{
  if (path_groups_)
    return path_groups_->findPathGroup(name, min_max);
  else
    return NULL;
}

PathGroup *
Search::findPathGroup(const Clock *clk,
		      const MinMax *min_max) const
{
  if (path_groups_)
    return path_groups_->findPathGroup(clk, min_max);
  else
    return NULL;
}

} // namespace