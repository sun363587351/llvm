//===- CompleteBottomUp.cpp - Complete Bottom-Up Data Structure Graphs ----===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This is the exact same as the bottom-up graphs, but we use take a completed
// call graph and inline all indirect callees into their callers graphs, making
// the result more useful for things like pool allocation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/DataStructure/DataStructure.h"
#include "llvm/Module.h"
#include "llvm/Analysis/DataStructure/DSGraph.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
using namespace llvm;

namespace {
  RegisterAnalysis<CompleteBUDataStructures>
  X("cbudatastructure", "'Complete' Bottom-up Data Structure Analysis");
  Statistic<> NumCBUInlines("cbudatastructures", "Number of graphs inlined");
}


// run - Calculate the bottom up data structure graphs for each function in the
// program.
//
bool CompleteBUDataStructures::runOnModule(Module &M) {
  BUDataStructures &BU = getAnalysis<BUDataStructures>();
  GlobalsGraph = new DSGraph(BU.getGlobalsGraph());
  GlobalsGraph->setPrintAuxCalls();

#if 1   // REMOVE ME EVENTUALLY
  // FIXME: TEMPORARY (remove once finalization of indirect call sites in the
  // globals graph has been implemented in the BU pass)
  TDDataStructures &TD = getAnalysis<TDDataStructures>();

  ActualCallees.clear();

  // The call graph extractable from the TD pass is _much more complete_ and
  // trustable than that generated by the BU pass so far.  Until this is fixed,
  // we hack it like this:
  for (Module::iterator MI = M.begin(), ME = M.end(); MI != ME; ++MI) {
    if (MI->isExternal()) continue;
    const std::list<DSCallSite> &CSs = TD.getDSGraph(*MI).getFunctionCalls();

    for (std::list<DSCallSite>::const_iterator CSI = CSs.begin(), E = CSs.end();
         CSI != E; ++CSI) {
      Instruction *TheCall = CSI->getCallSite().getInstruction();

      if (CSI->isIndirectCall()) { // indirect call: insert all callees
        const std::vector<GlobalValue*> &Callees =
          CSI->getCalleeNode()->getGlobals();
        for (unsigned i = 0, e = Callees.size(); i != e; ++i)
          if (Function *F = dyn_cast<Function>(Callees[i]))
            ActualCallees.insert(std::make_pair(TheCall, F));
      } else {        // direct call: insert the single callee directly
        ActualCallees.insert(std::make_pair(TheCall,
                                            CSI->getCalleeFunc()));
      }
    }
  }
#else
  // Our call graph is the same as the BU data structures call graph
  ActualCallees = BU.getActualCallees();
#endif

  std::vector<DSGraph*> Stack;
  hash_map<DSGraph*, unsigned> ValMap;
  unsigned NextID = 1;

  Function *MainFunc = M.getMainFunction();
  if (MainFunc) {
    if (!MainFunc->isExternal())
      calculateSCCGraphs(getOrCreateGraph(*MainFunc), Stack, NextID, ValMap);
  } else {
    std::cerr << "CBU-DSA: No 'main' function found!\n";
  }
  
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isExternal() && !DSInfo.count(I))
      calculateSCCGraphs(getOrCreateGraph(*I), Stack, NextID, ValMap);

  GlobalsGraph->removeTriviallyDeadNodes();


  // Merge the globals variables (not the calls) from the globals graph back
  // into the main function's graph so that the main function contains all of
  // the information about global pools and GV usage in the program.
  if (MainFunc && !MainFunc->isExternal()) {
    DSGraph &MainGraph = getOrCreateGraph(*MainFunc);
    const DSGraph &GG = *MainGraph.getGlobalsGraph();
    ReachabilityCloner RC(MainGraph, GG, 
                          DSGraph::DontCloneCallNodes |
                          DSGraph::DontCloneAuxCallNodes);

    // Clone the global nodes into this graph.
    for (DSScalarMap::global_iterator I = GG.getScalarMap().global_begin(),
           E = GG.getScalarMap().global_end(); I != E; ++I)
      if (isa<GlobalVariable>(*I))
        RC.getClonedNH(GG.getNodeForValue(*I));

    MainGraph.maskIncompleteMarkers();
    MainGraph.markIncompleteNodes(DSGraph::MarkFormalArgs | 
                                  DSGraph::IgnoreGlobals);
  }

  return false;
}

DSGraph &CompleteBUDataStructures::getOrCreateGraph(Function &F) {
  // Has the graph already been created?
  DSGraph *&Graph = DSInfo[&F];
  if (Graph) return *Graph;

  // Copy the BU graph...
  Graph = new DSGraph(getAnalysis<BUDataStructures>().getDSGraph(F));
  Graph->setGlobalsGraph(GlobalsGraph);
  Graph->setPrintAuxCalls();

  // Make sure to update the DSInfo map for all of the functions currently in
  // this graph!
  for (DSGraph::retnodes_iterator I = Graph->retnodes_begin();
       I != Graph->retnodes_end(); ++I)
    DSInfo[I->first] = Graph;

  return *Graph;
}



unsigned CompleteBUDataStructures::calculateSCCGraphs(DSGraph &FG,
                                                  std::vector<DSGraph*> &Stack,
                                                  unsigned &NextID, 
                                         hash_map<DSGraph*, unsigned> &ValMap) {
  assert(!ValMap.count(&FG) && "Shouldn't revisit functions!");
  unsigned Min = NextID++, MyID = Min;
  ValMap[&FG] = Min;
  Stack.push_back(&FG);

  // The edges out of the current node are the call site targets...
  for (DSGraph::fc_iterator CI = FG.fc_begin(), CE = FG.fc_end();
       CI != CE; ++CI) {
    Instruction *Call = CI->getCallSite().getInstruction();

    // Loop over all of the actually called functions...
    ActualCalleesTy::iterator I, E;
    for (tie(I, E) = ActualCallees.equal_range(Call); I != E; ++I)
      if (!I->second->isExternal()) {
        DSGraph &Callee = getOrCreateGraph(*I->second);
        unsigned M;
        // Have we visited the destination function yet?
        hash_map<DSGraph*, unsigned>::iterator It = ValMap.find(&Callee);
        if (It == ValMap.end())  // No, visit it now.
          M = calculateSCCGraphs(Callee, Stack, NextID, ValMap);
        else                    // Yes, get it's number.
          M = It->second;
        if (M < Min) Min = M;
      }
  }

  assert(ValMap[&FG] == MyID && "SCC construction assumption wrong!");
  if (Min != MyID)
    return Min;         // This is part of a larger SCC!

  // If this is a new SCC, process it now.
  bool IsMultiNodeSCC = false;
  while (Stack.back() != &FG) {
    DSGraph *NG = Stack.back();
    ValMap[NG] = ~0U;

    DSGraph::NodeMapTy NodeMap;
    FG.cloneInto(*NG, FG.getScalarMap(), FG.getReturnNodes(), NodeMap);

    // Update the DSInfo map and delete the old graph...
    for (DSGraph::retnodes_iterator I = NG->retnodes_begin();
         I != NG->retnodes_end(); ++I)
      DSInfo[I->first] = &FG;

    // Remove NG from the ValMap since the pointer may get recycled.
    ValMap.erase(NG);
    delete NG;
    
    Stack.pop_back();
    IsMultiNodeSCC = true;
  }

  // Clean up the graph before we start inlining a bunch again...
  if (IsMultiNodeSCC)
    FG.removeTriviallyDeadNodes();
  
  Stack.pop_back();
  processGraph(FG);
  ValMap[&FG] = ~0U;
  return MyID;
}


/// processGraph - Process the BU graphs for the program in bottom-up order on
/// the SCC of the __ACTUAL__ call graph.  This builds "complete" BU graphs.
void CompleteBUDataStructures::processGraph(DSGraph &G) {
  hash_set<Instruction*> calls;

  // The edges out of the current node are the call site targets...
  unsigned i = 0;
  for (DSGraph::fc_iterator CI = G.fc_begin(), CE = G.fc_end(); CI != CE;
       ++CI, ++i) {
    const DSCallSite &CS = *CI;
    Instruction *TheCall = CS.getCallSite().getInstruction();

    assert(calls.insert(TheCall).second &&
           "Call instruction occurs multiple times in graph??");
      

    // Loop over all of the potentially called functions...
    // Inline direct calls as well as indirect calls because the direct
    // callee may have indirect callees and so may have changed.
    // 
    ActualCalleesTy::iterator I, E;
    tie(I, E) = ActualCallees.equal_range(TheCall);
    unsigned TNum = 0, Num = std::distance(I, E);
    for (; I != E; ++I, ++TNum) {
      Function *CalleeFunc = I->second;
      if (!CalleeFunc->isExternal()) {
        // Merge the callee's graph into this graph.  This works for normal
        // calls or for self recursion within an SCC.
        DSGraph &GI = getOrCreateGraph(*CalleeFunc);
        ++NumCBUInlines;
        G.mergeInGraph(CS, *CalleeFunc, GI, DSGraph::KeepModRefBits |
                       DSGraph::StripAllocaBit | DSGraph::DontCloneCallNodes |
                       DSGraph::DontCloneAuxCallNodes);
        DEBUG(std::cerr << "    Inlining graph [" << i << "/"
              << G.getFunctionCalls().size()-1
              << ":" << TNum << "/" << Num-1 << "] for "
              << CalleeFunc->getName() << "["
              << GI.getGraphSize() << "+" << GI.getAuxFunctionCalls().size()
              << "] into '" /*<< G.getFunctionNames()*/ << "' ["
              << G.getGraphSize() << "+" << G.getAuxFunctionCalls().size()
              << "]\n");
      }
    }
  }

  // Recompute the Incomplete markers
  assert(G.getInlinedGlobals().empty());
  G.maskIncompleteMarkers();
  G.markIncompleteNodes(DSGraph::MarkFormalArgs);

  // Delete dead nodes.  Treat globals that are unreachable but that can
  // reach live nodes as live.
  G.removeDeadNodes(DSGraph::KeepUnreachableGlobals);
}
