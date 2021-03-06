/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 ******************************************************************************/

#include "il/OMRBlock.hpp"

#include <limits.h>                            // for INT_MAX
#include <stdint.h>                            // for int32_t, uint32_t, etc
#include <string.h>                            // for NULL, strncmp
#include <stdio.h>                             // for sprintf
#include "codegen/CodeGenerator.hpp"           // for CodeGenerator
#include "codegen/FrontEnd.hpp"                // for TR_FrontEnd
#include "compile/Compilation.hpp"             // for Compilation
#include "compile/CompilationTypes.hpp"        // for TR_Hotness
#include "compile/Method.hpp"                  // for TR_Method
#include "compile/ResolvedMethod.hpp"          // for TR_ResolvedMethod
#include "compile/SymbolReferenceTable.hpp"    // for SymbolReferenceTable
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "cs2/sparsrbit.h"
#include "env/CompilerEnv.hpp"
#include "env/TRMemory.hpp"                    // for SparseBitVector, etc
#include "env/jittypes.h"                      // for TR_ByteCodeInfo, etc
#include "il/AliasSetInterface.hpp"
#include "il/Block.hpp"                        // for Block
#include "il/Block_inlines.hpp"                // for Block::self
#include "il/DataTypes.hpp"                    // for DataTypes::NoType, etc
#include "il/ILOpCodes.hpp"                    // for ILOpCodes
#include "il/ILOps.hpp"                        // for ILOpCode
#include "il/Node.hpp"                         // for Node, etc
#include "il/Node_inlines.hpp"                 // for Node::getChild, etc
#include "il/Symbol.hpp"                       // for Symbol
#include "il/SymbolReference.hpp"              // for SymbolReference, etc
#include "il/TreeTop.hpp"                      // for TreeTop
#include "il/TreeTop_inlines.hpp"              // for TreeTop::getNode, etc
#include "il/symbol/LabelSymbol.hpp"           // for LabelSymbol
#include "il/symbol/RegisterMappedSymbol.hpp"  // for RegisterMappedSymbol
#include "il/symbol/ResolvedMethodSymbol.hpp"  // for ResolvedMethodSymbol
#include "infra/Array.hpp"                     // for TR_Array
#include "infra/Assert.hpp"                    // for TR_ASSERT
#include "infra/BitVector.hpp"                 // for TR_BitVector
#include "infra/Cfg.hpp"                       // for CFG, MAX_BLOCK_COUNT, etc
#include "infra/Flags.hpp"                     // for flags32_t
#include "infra/Link.hpp"                      // for TR_LinkHeadAndTail, etc
#include "infra/List.hpp"                      // for ListIterator, List, etc
#include "infra/TRCfgEdge.hpp"                 // for CFGEdge
#include "infra/TRCfgNode.hpp"                 // for CFGNode
#include "optimizer/Inliner.hpp"
#include "optimizer/Optimizer.hpp"             // for Optimizer
#include "optimizer/RegisterCandidate.hpp"     // for TR_GlobalRegister
#include "optimizer/Structure.hpp"             // for TR_BlockStructure, etc
#include "ras/Debug.hpp"                       // for TR_DebugBase

class TR_Memory;
namespace TR { class DebugCounterAggregation; }
namespace TR { class Instruction; }

TR::Block *
OMR::Block::createEmptyBlock(TR::Compilation *comp, int32_t frequency, TR::Block *block)
   {
   if (!comp->isPeekingMethod() && (comp->getFlowGraph()->getMaxFrequency() >= 0))
      TR_ASSERT((frequency >= 0), "Block frequency must be non negative\n");
   return TR::Block::createEmptyBlock(0, comp, frequency, block);
   }

TR::Block *
OMR::Block::asBlock() { return self(); }

OMR::Block::Block(TR_Memory * m) :
   TR::CFGNode(m),
   _pEntry(NULL),
   _pExit(NULL),
   _pStructureOf(NULL),
   _liveLocals(NULL),
   _globalRegisters(0),
   _registerSaveDescriptionBits(0),
   _catchBlockExtension(NULL),
   _firstInstruction(NULL),
   _lastInstruction(NULL),
   _blockSize(-1),
   _debugCounters(NULL),
   _flags(0),
   _moreflags(0)
   {
   self()->setFrequency(-1);
   self()->setUnrollFactor(0);
   }

OMR::Block::Block(TR::TreeTop *entry, TR::TreeTop *exit, TR_Memory * m) :
   TR::CFGNode(m),
   _pEntry(entry),
   _pExit(exit),
   _pStructureOf(NULL),
   _liveLocals(NULL),
   _globalRegisters(0),
   _registerSaveDescriptionBits(0),
   _catchBlockExtension(NULL),
   _firstInstruction(NULL),
   _lastInstruction(NULL),
   _blockSize(-1),
   _debugCounters(NULL),
   _flags(0),
   _moreflags(0)
   {
   self()->setFrequency(-1);
   self()->setUnrollFactor(0);
   if (entry && entry->getNode()) entry->getNode()->setBlock(self());
   if (exit && exit->getNode())   exit->getNode()->setBlock(self());
   }

/// Copy constructor
OMR::Block::Block(TR::Block &other, TR::TreeTop *entry, TR::TreeTop *exit) :
   TR::CFGNode(other.trMemory()),
   _pEntry(entry),
   _pExit(exit),
   _pStructureOf(NULL),
   _liveLocals(NULL),
   _globalRegisters(0),
   _firstInstruction(other._firstInstruction),
   _lastInstruction(other._lastInstruction),
   _blockSize(other._blockSize),
   _debugCounters(other._debugCounters),
   _catchBlockExtension(NULL)
   {
   if (entry && entry->getNode()) entry->getNode()->setBlock(self());
   if (exit && exit->getNode())   exit->getNode()->setBlock(self());

   if (other._liveLocals)
      _liveLocals = new (other.trHeapMemory()) TR_BitVector(*other._liveLocals);

   if (other._catchBlockExtension)
      {
      _catchBlockExtension = new (other.trHeapMemory())
         TR_CatchBlockExtension(*other._catchBlockExtension);
      }

   self()->setFrequency(other.getFrequency());

   if (other._globalRegisters)
      _globalRegisters = new (other.trHeapMemory()) TR_Array<TR_GlobalRegister>(*other._globalRegisters);

   _flags.set(other._flags.getValue());
   _moreflags.set(other._moreflags.getValue());
   _unrollFactor = other.getUnrollFactor();
   }

TR::Block *
OMR::Block::getNextBlock()
   {
   TR::TreeTop * t = _pExit->getNextTreeTop();
   return t ? t->getNode()->getBlock() : 0;
   }

TR::Block *
OMR::Block::getPrevBlock()
   {
   TR::TreeTop * t = _pEntry->getPrevTreeTop();
   return t ? t->getNode()->getBlock() : 0;
   }

TR::Block *
OMR::Block::getNextExtendedBlock()
   {
   TR::Block * b = self()->getNextBlock();
   for (; b && b->isExtensionOfPreviousBlock(); b = b->getNextBlock())
      ;
   return b;
   }


// append a node under a treetop and before the bbend
TR::TreeTop *
OMR::Block::append(TR::TreeTop * tt)
   {
   return self()->getExit()->insertBefore(tt);
   }

/**
 * Prepend a OMR::Block::node under a treetop immediately after the bbStart
 */
TR::TreeTop *
OMR::Block::prepend(TR::TreeTop * tt)
   {
   return self()->getEntry()->insertAfter(tt);
   }

TR::TreeTop *
OMR::Block::getFirstRealTreeTop()
   {
   TR_ASSERT(self()->getEntry() && self()->getEntry()->getNextTreeTop(), "Block must have valid entry treetop\n");
   TR::TreeTop * tt = self()->getEntry()->getNextTreeTop();
   while (tt->getNode()->getOpCode().isExceptionRangeFence())
      tt = tt->getNextTreeTop();
   return tt;
   }

TR::TreeTop *
OMR::Block::getLastRealTreeTop()
   {
   TR_ASSERT(self()->getExit() && self()->getExit()->getPrevTreeTop(), "Block must have valid exit treetop\n");
   TR::TreeTop * tt = self()->getExit()->getPrevTreeTop();
   while (tt->getNode()->getOpCode().isExceptionRangeFence())
      tt = tt->getPrevTreeTop();
   return tt;
   }

TR::TreeTop *
OMR::Block::getLastNonControlFlowTreeTop()
   {
   TR::TreeTop *tt = self()->getLastRealTreeTop();
   while (tt->getNode()->getOpCode().isBranch() ||
          tt->getNode()->getOpCode().isReturn() ||
          tt->getNode()->getOpCode().isJumpWithMultipleTargets())
      {
      tt = tt->getPrevTreeTop();
      }
   return tt;
   }

int32_t
OMR::Block::getNumberOfRealTreeTops()
   {
   TR_ASSERT(self()->getEntry(), "Block must have valid entry treetop\n");
   int32_t numRealTreeTops = 0;
   TR::TreeTop * tt = self()->getEntry()->getNextRealTreeTop();
   TR::TreeTop *exitTree = self()->getExit();
   while (tt != exitTree)
      {
      tt = tt->getNextRealTreeTop();
      numRealTreeTops++;
      }
   return numRealTreeTops;
   }


TR::Block *
OMR::Block::createEmptyBlock(TR::Node * n, TR::Compilation *comp, int32_t frequency, TR::Block *block)
   {
   if (!comp->isPeekingMethod() && (comp->getFlowGraph()->getMaxFrequency() >= 0))
      {
      TR_ASSERT((frequency >= 0), "Block frequency must be non negative\n");
      }

   if (block!=NULL) comp->setCurrentBlock(block);

   TR::Block * b =
      new (comp->trHeapMemory()) TR::Block(
         TR::TreeTop::create(comp, TR::Node::create(n, TR::BBStart)),
         TR::TreeTop::create(comp, TR::Node::create(n, TR::BBEnd)),
         comp->trMemory());
   b->getEntry()->join(b->getExit());
   b->setFrequency(frequency);
   return b;
   }

int32_t
OMR::Block::getMaxColdFrequency(TR::Block *b1, TR::Block *b2)
   {
   int32_t max = -1;
   int32_t frequency = b1->getFrequency();
   if (b1->isCold())
      max = frequency;
   frequency = b2->getFrequency();
   if (b2->isCold())
      {
      if (frequency > max)
         max = frequency;
      }
   //TR_ASSERT((max > -1), "At least one of the blocks should be cold\n");
   return max;
   }

int32_t
OMR::Block::getMinColdFrequency(TR::Block *b1, TR::Block *b2)
   {
   int32_t min = INT_MAX;
   int32_t frequency = b1->getFrequency();
   if (b1->isCold())
      min = frequency;
   frequency = b2->getFrequency();
   if (b2->isCold())
      {
      if (frequency < min)
         min = frequency;
      }
   TR_ASSERT((min < INT_MAX), "At least one of the blocks should be cold\n");
   return min;
   }

int32_t
OMR::Block::getScaledSpecializedFrequency(int32_t fastFrequency)
   {
   return (int32_t) ((float) (1.0f - MIN_PROFILED_FREQUENCY)*fastFrequency);
   }

bool
OMR::Block::isGotoBlock(TR::Compilation * comp, bool allowPrecedingSnapshots)
   {
   bool isGoToBlock = (   self()->getEntry() != NULL
                       && self()->getLastRealTreeTop()->getPrevTreeTop() == self()->getEntry()
                       && self()->getLastRealTreeTop()->getNode()->getOpCodeValue() == TR::Goto);
   if (!isGoToBlock && allowPrecedingSnapshots && comp->getOption(TR_EnableSnapshotBlockOpts) && comp->getMethodSymbol()->hasSnapshots())
      {
      if (self()->getEntry() != NULL && self()->getLastRealTreeTop()->getNode()->getOpCodeValue() == TR::Goto)
         {
         for (TR::TreeTop *tt = self()->getLastRealTreeTop()->getPrevTreeTop(); tt != self()->getEntry(); tt = tt->getPrevTreeTop())
            {
            return false;
            }
         isGoToBlock = true;
         }
      }
   return isGoToBlock;
   }

bool
OMR::Block::canFallThroughToNextBlock()
   {
   TR::Node *lastNode = self()->getLastRealTreeTop()->getNode();
   if (lastNode->getOpCodeValue() == TR::treetop)
      lastNode = lastNode->getFirstChild();
   if (lastNode->getOpCode().isGoto() ||
       lastNode->getOpCode().isReturn() ||
       lastNode->getOpCodeValue() == TR::athrow ||
       lastNode->getOpCodeValue() == TR::igoto)
      return false;
   return true;
   }

/**
 * Here we will detect the case where we have a label at BBStart but that will never be used.
 * If the only predecessor to the block belonging to the BBStart is the previous block that means that
 * we will fallthrough to this block without a branch, and therefore we don't need a label. There are
 * some exceptions:
 *   - If the previous block is a lookup, table or tstart block, then we might have a branch
 *     instruction to this block even if this block is the fallthrough block
 *   - if "canFallThroughToNextBlock()" returns false, we abort (it checks for similar situations)
 */
bool
OMR::Block::doesNotNeedLabelAtStart()
   {

   if (!self()->getEntry()->getNode()->getLabel())
      return true;

   if (self()->getEntry()->getNode()->getLabel()->isHasAddrTaken())
      return false;

   for (auto thisEdge = self()->getPredecessors().begin(); thisEdge != self()->getPredecessors().end(); ++thisEdge)
      {
      TR::Block * from = (*thisEdge)->getFrom()->asBlock();
      TR::Block * prevBlock = self()->getPrevBlock();
      if (from == prevBlock && from->canFallThroughToNextBlock())
         {
         TR::Node *lastNode = from->getLastRealTreeTop()->getNode();
         if (lastNode->getOpCodeValue() == TR::treetop)
            lastNode = lastNode->getFirstChild();
         if (lastNode->getOpCodeValue() != TR::lookup &&
             lastNode->getOpCodeValue() != TR::table &&
             lastNode->getOpCodeValue() != TR::tstart &&
             lastNode->getOpCodeValue() != TR::trtLookup)
            continue;
         }
      return false;
      }

   return true;
   }


/**
 * Return fall through edge within a Ebb if any.
 * Can handle dummy block created during inlining (callee has dummy nodes).
 */
TR::CFGEdge *
OMR::Block::getFallThroughEdgeInEBB()
   {
   TR::Block *fallThruCandidate = self()->getExit() ? self()->getNextBlock() : 0;
   if (fallThruCandidate && fallThruCandidate->isExtensionOfPreviousBlock())
      return self()->getEdge(fallThruCandidate);
   return 0;
   }



TR::Block *
OMR::Block::breakFallThrough(TR::Compilation *comp, TR::Block *faller, TR::Block *fallee)
   {
   TR::Node *lastNode = faller->getLastRealTreeTop()->getNode();
   if (lastNode->getOpCode().isCheck() || lastNode->getOpCodeValue() == TR::treetop)
      lastNode = lastNode->getFirstChild();

   if (lastNode->getOpCode().isReturn() ||
       lastNode->getOpCode().isGoto()   ||
       lastNode->getOpCode().isSwitch() ||
       lastNode->getOpCode().isJumpWithMultipleTargets() ||
       lastNode->getOpCodeValue() == TR::athrow)
      return faller; // nothing to do

   if (lastNode->getOpCode().isBranch())
      {
      TR::Node    *gotoNode = TR::Node::create(lastNode, TR::Goto);
      TR::TreeTop *gotoTree = TR::TreeTop::create(comp, gotoNode);
      gotoNode->setBranchDestination(fallee->getEntry());
      TR::Block  *gotoBlock = TR::Block::createEmptyBlock(lastNode, comp, (fallee->getFrequency() < faller->getFrequency()) ? fallee->getFrequency() : faller->getFrequency(), fallee);
      gotoBlock->append(gotoTree);

      faller->getExit()->join(gotoBlock->getEntry());
      gotoBlock->getExit()->join(fallee->getEntry());

      if (faller->getStructureOf())
         comp->getFlowGraph()->addNode(gotoBlock, faller->getCommonParentStructureIfExists(fallee, comp->getFlowGraph()));
      else
         comp->getFlowGraph()->addNode(gotoBlock);
      comp->getFlowGraph()->addEdge(TR::CFGEdge::createEdge(faller,  gotoBlock, self()->trMemory()));
      comp->getFlowGraph()->addEdge(TR::CFGEdge::createEdge(gotoBlock,  fallee, self()->trMemory()));
      // remove the edge only if the branch target is not pointing to the next block
      if (lastNode->getBranchDestination() != fallee->getEntry())
         comp->getFlowGraph()->removeEdge(faller, fallee);
      if (fallee->isCold() || faller->isCold())
         {
         //gotoBlock->setIsCold();
         if (fallee->isCold())
            gotoBlock->setFrequency(fallee->getFrequency());
         else
            gotoBlock->setFrequency(faller->getFrequency());
         }
      return gotoBlock;
      }

   TR::Node    *gotoNode = TR::Node::create(lastNode, TR::Goto);
   TR::TreeTop *gotoTree = TR::TreeTop::create(comp, gotoNode);
   gotoNode->setBranchDestination(fallee->getEntry());
   faller->append(gotoTree);
   return faller;
   }


/**
 * @note insertBlockAsFallThrough creates a new edge from block -> newFallThroughBlock.  It does _NOT_
 *       remove any fall-through edge that might have previously existed from block.  That responsibility
 *       is left to the caller since removing the edge at the wrong time could leave the original fallthrough
 *       successor with no predecessors which could leave it in an unexpectedly deleted state.
 */
void
OMR::Block::insertBlockAsFallThrough(TR::Compilation *comp, TR::Block *block, TR::Block *newFallThroughBlock)
   {
   TR::TreeTop *origFallThroughEntry = block->getExit()->getNextRealTreeTop();
   TR::TreeTop *newFallThroughEntry = newFallThroughBlock->getEntry();
   TR::TreeTop *beforeNewFallThrough = newFallThroughEntry->getPrevTreeTop();
   TR::TreeTop *afterNewFallThrough = newFallThroughBlock->getExit()->getNextTreeTop();
   TR::TreeTop *beforeOrigFallThrough = block->getExit();

   // rip newFallThrough block out of trees
   beforeNewFallThrough->join(afterNewFallThrough);

   // now insert newFallThrough between end of block and its original fall-through successor
   TR::TreeTop::insertTreeTops(comp, block->getExit(), newFallThroughBlock->getEntry(), newFallThroughBlock->getExit());
   if (!block->hasSuccessor(newFallThroughBlock))
      comp->getFlowGraph()->addEdge(block, newFallThroughBlock);
   }


/**
 * comment about when edge from from -> origTo will be removed (clients should be careful that origTo doesn't unexpectedly go poof!)
 * @note  redirectFlowToNewDestination will both create a new edge from "from" to "newTo" as well as remove the original edge from
 *        "from" to origTo.  BE CAREFUL when calling this function that there is another predecessor edge for "origTo" or else when
 *        redirectFlowToNewDestination removes the predecessor "from", "origTo" might be deleted.
 */
void
OMR::Block::redirectFlowToNewDestination(TR::Compilation *comp, TR::CFGEdge *origEdge, TR::Block *newTo, bool useGotoForFallThrough)
   {
   TR::Block *from = origEdge->getFrom()->asBlock();
   TR::Block *origTo = origEdge->getTo()->asBlock();
   int32_t fromBlockNumber = from->getNumber();
   int32_t origToBlockNumber = origTo->getNumber();
   int32_t newToBlockNumber = newTo->getNumber();
   if (from->getEntry() == NULL)
      {
      // from is entry, so only have to change the flow edges (no branch to change)
      if (!from->hasSuccessor(newTo))
         comp->getFlowGraph()->addEdge(from, newTo);
      comp->getFlowGraph()->removeEdge(from, origTo);
      }
   else
      {
      TR::TreeTop *lastTreeTop = from->getLastRealTreeTop();
      TR_ASSERT(lastTreeTop != NULL, "how can there be an entry but no last tree top?");
      TR::Node *lastNode = lastTreeTop->getNode();
      if (lastNode->getOpCode().isBranch() && lastNode->getBranchDestination() == origTo->getEntry())
         {
         // need to change the branch destination from origTo to newTo
         from->changeBranchDestination(newTo->getEntry(), comp->getFlowGraph());
         }
      else if (lastNode->getOpCode().isSwitch())
         {
         TR::TreeTop *origToEntry = origTo->getEntry();
         TR::TreeTop *newToEntry = newTo->getEntry();

         // find correct switch target(s) and change it to newTo->getNumber()
         for (int i=lastNode->getCaseIndexUpperBound()-1; i > 0; i--)
            {
            TR::Node *child=lastNode->getChild(i);
            if (child->getBranchDestination() == origToEntry)
               child->setBranchDestination(newToEntry);
            }
         if (!from->hasSuccessor(newTo))
            comp->getFlowGraph()->addEdge(from, newTo);
         comp->getFlowGraph()->removeEdge(from, origTo);
         }
      else if (lastNode->getOpCode().isJumpWithMultipleTargets())
         {
         if (lastNode->getOpCode().hasBranchChildren())
            {
            traceMsg(comp, "Jump with multple targets, with non fall through path to empty block\n");
            TR::TreeTop *origToEntry = origTo->getEntry();
            TR::TreeTop *newToEntry = newTo->getEntry();
            if(origToEntry)
               traceMsg(comp, "jumpwithmultipletargets: origToEntry->getNode = %p\n",origToEntry->getNode());
            if(newToEntry)
               traceMsg(comp, "jumpwithmultipletargets: newToEntry->getNode = %p\n",newToEntry->getNode());
            // Get the right branch target
            for (int32_t i=0 ; i < lastNode->getNumChildren()-1 ; i++)
               {
               TR::Node *child = lastNode->getChild(i);
               traceMsg(comp, "considering node %p with branch destination %p \n",child,child->getBranchDestination() ? child->getBranchDestination()->getNode() : 0 );
               if (child->getBranchDestination() == origToEntry)
                  {
                  child->setBranchDestination(newToEntry);
                  if (!from->hasSuccessor(newTo))
                     comp->getFlowGraph()->addEdge(from, newTo);
                  comp->getFlowGraph()->removeEdge(from, origTo);
                  }
               }
            }
         else
            {
            if (!from->hasSuccessor(newTo))
               comp->getFlowGraph()->addEdge(from, newTo);
            comp->getFlowGraph()->removeEdge(from, origTo);
            }
         }
      else
         // from falls through to origTo
         {
         TR::TreeTop *origToEntry = origTo->getEntry();
         TR::TreeTop *newToEntry = newTo->getEntry();
         TR::TreeTop *afterNewTo = newTo->getExit()->getNextTreeTop();
         TR_ASSERT(from->getExit()->getNextTreeTop() == origToEntry, "Expecting from's fallthrough to be origTo");

         if (useGotoForFallThrough)
            {
            TR::TreeTop *gotoTreeTop = TR::TreeTop::create(comp, TR::Node::create(lastNode, TR::Goto, 0, newTo->getEntry()));
            if (lastNode->getOpCode().isBranch())
               {
               int32_t freq = origEdge->getFrequency();

               TR::Block *newGotoBlock = TR::Block::createEmptyBlock(lastNode, comp, freq, from);
               newGotoBlock->append(gotoTreeTop);
               comp->getFlowGraph()->addNode(newGotoBlock);
               comp->getFlowGraph()->addEdge(from, newGotoBlock)->setFrequency(freq);
               newGotoBlock->setIsExtensionOfPreviousBlock();
               TR::Block::insertBlockAsFallThrough(comp, from, newGotoBlock);  // edge won't be duplicated

               comp->getFlowGraph()->addEdge(newGotoBlock, newTo)->setFrequency(freq);
               comp->getFlowGraph()->removeEdge(from, origTo);
               }
            else
               {
               from->append(gotoTreeTop);
               if (!from->hasSuccessor(newTo))
                  comp->getFlowGraph()->addEdge(from, newTo);
               comp->getFlowGraph()->removeEdge(from, origTo);
               }
            }
         else
            {
            TR::Block::insertBlockAsFallThrough(comp, from, newTo);
            comp->getFlowGraph()->removeEdge(from, origTo);
            }
         }
      }
   }

/**
 * This block is being removed from the CFG.
 * Remove its trees from the list of trees.
 */
void
OMR::Block::removeFromCFG(TR::Compilation *c)
   {
   // Remove the trees for this block
   //
   if (_pEntry != NULL)
      {

      // Remove trees
      for (TR::TreeTop *treeTop = _pEntry, *next; ; treeTop = next)
         {
         next = treeTop->getNextTreeTop();
         c->removeTree(treeTop);
         if (treeTop == _pExit)
            break;
         }
      }

   }


TR_RegionStructure *
OMR::Block::getParentStructureIfExists(TR::CFG *cfg)
   {
   TR_RegionStructure *parent = NULL;
   if (self()->getStructureOf() &&
       cfg->getStructure())
      parent = self()->getStructureOf()->getParent()->asRegion();

   return parent;
   }

TR_RegionStructure *
OMR::Block::getCommonParentStructureIfExists(TR::Block *b, TR::CFG *cfg)
   {
   TR_RegionStructure *parent = NULL;
   if (self()->getStructureOf() &&
       b &&
       b->getStructureOf() &&
       cfg->getStructure())
      parent = self()->getStructureOf()->findCommonParent(b->getStructureOf(), cfg);

   return parent;
   }

void
OMR::Block::removeBranch(TR::Compilation *c)
   {
   TR::TreeTop * tt = self()->getLastRealTreeTop();
   TR_ASSERT(tt->getNode()->getOpCode().isBranch(), "OMR::Block::removeBranch, block doesn't have a branch");
   c->getFlowGraph()->removeEdge(self(), tt->getNode()->getBranchDestination()->getNode()->getBlock());
   c->removeTree(tt);
   }

TR::Block *
OMR::Block::findVirtualGuardBlock(TR::CFG *cfg)
   {
   //TR_ASSERT(getExceptionPredecessors().empty(), "snippet block has an exception predecessor");
   for (auto nextEdge = self()->getPredecessors().begin(); nextEdge != self()->getPredecessors().end(); ++nextEdge)
      {
      TR::Block *prevBlock = toBlock((*nextEdge)->getFrom());
      // check for the entry block being the predecessor
      // ie. the guard has probably been removed
      if (prevBlock == cfg->getStart())
         break;
      //else if (prevBlock->getLastRealTreeTop()->getNode()->isTheVirtualGuardForAGuardedInlinedCall())
      //   return prevBlock;
      else
         {
         TR::Node *node = prevBlock->getLastRealTreeTop()->getNode();
         if (node->isTheVirtualGuardForAGuardedInlinedCall())
            {
            // extra check for profiled guards based on VFT
            if (node->isProfiledGuard())
               {
               TR_ASSERT(((node->getOpCodeValue() == TR::ifacmpne) || (node->getOpCodeValue() == TR::ifacmpeq)), "Profile guards must use TR::ifacmpne || TR::ifacmpeq");
               if (node->getOpCodeValue() == TR::ifacmpne)
                  {
                  if (node->getBranchDestination()->getEnclosingBlock() == self())
                     return prevBlock;
                  }
               else if (node->getOpCodeValue() == TR::ifacmpeq)
                  {
                  if (prevBlock->getNextBlock() == self())
                     return prevBlock;
                  }
               }
            else
               {
               return prevBlock;
               }
            }
         }
      }
   return 0;
   }

void
OMR::Block::changeBranchDestination(TR::TreeTop * newDestination, TR::CFG *cfg)
   {

   TR::Node * branchNode = self()->getLastRealTreeTop()->getNode();
   TR_ASSERT(branchNode->getOpCode().isBranch(), "OMR::Block::changeBranchDestination: can't find the existing branch node");

   TR::Block * prevDestinationBlock = branchNode->getBranchDestination()->getNode()->getBlock();
   branchNode->setBranchDestination(newDestination);

   TR::Block * newDestinationBlock = newDestination->getNode()->getBlock();
   TR::CFGEdge *prevEdge = self()->getEdge(prevDestinationBlock);
   if (!self()->hasSuccessor(newDestinationBlock))
      {
      TR::CFGEdge *newEdge = cfg->addEdge(self(), newDestinationBlock);
      int32_t prevEdgeFrequency = prevEdge->getFrequency();
      if (prevDestinationBlock->getFrequency() > 0)
         {
         int32_t newEdgeFrequency = (prevEdgeFrequency*newDestinationBlock->getFrequency())/prevDestinationBlock->getFrequency();
         newEdge->setFrequency(newEdgeFrequency);
         }
      }

   cfg->removeEdge(prevEdge);

   int32_t numChildren = branchNode->getNumChildren();
   if (numChildren >0 &&
      branchNode->getChild(numChildren - 1)->getOpCodeValue() == TR::GlRegDeps)
      {
      TR::Node * fromGlRegDep = branchNode->getChild(numChildren - 1);
      TR_ASSERT(fromGlRegDep->getOpCodeValue() == TR::GlRegDeps, "expected the branch child to be a GlRegDep.  It's not!");
      int32_t numFromGlRegDeps = fromGlRegDep->getNumChildren();
      if (newDestination->getNode()->getNumChildren() == 0)
         {
         branchNode->setNumChildren(numChildren - 1);
         for (int32_t i = 0; i < numFromGlRegDeps; ++i)
            fromGlRegDep->getChild(i)->recursivelyDecReferenceCount();
         }
      else
         {
         TR::Node * toGlRegDep = newDestination->getNode()->getChild(0);
         TR_ASSERT(toGlRegDep->getOpCodeValue() == TR::GlRegDeps, "expected the bbstart child to be a GlRegDep.  It's not!");
   TR_ASSERT(0, "todo: update the GlRegDep info");
         }
      }
   else
      {
      TR_ASSERT(newDestination->getNode()->getNumChildren() == 0, "todo: update the GlRegDep info");
      }
   }

void
OMR::Block::uncommonNodesBetweenBlocks(TR::Compilation *comp, TR::Block *newBlock)
   {
   TR_ScratchList<TR::SymbolReference> symbolReferenceTempsA(self()->trMemory());
   TR_ScratchList<TR::SymbolReference> injectedBasicBlockTemps(self()->trMemory());
   TR_ScratchList<TR::SymbolReference> symbolReferenceTempsC(self()->trMemory());

   TR_HandleInjectedBasicBlock ibb(comp, NULL, comp->getMethodSymbol(),
                                   symbolReferenceTempsA,
                                   injectedBasicBlockTemps,
                                   symbolReferenceTempsC);
   ibb.findAndReplaceReferences(self()->getEntry(), newBlock, NULL);

   ListIterator<TR::SymbolReference> injectedBasicBlockTempIter(&injectedBasicBlockTemps);
   TR::SymbolReference * injectedBasicBlockTemp = injectedBasicBlockTempIter.getFirst();

   for (; injectedBasicBlockTemp; injectedBasicBlockTemp = injectedBasicBlockTempIter.getNext())
      {
      comp->getMethodSymbol()->addAutomatic(injectedBasicBlockTemp->getSymbol()->castToAutoSymbol());
      }
   }

bool
OMR::Block::hasExceptionSuccessors()
   {
   bool hasExceptionSuccessors = (self()->getExceptionSuccessors().empty() == false);
   return hasExceptionSuccessors;
   }

bool
OMR::Block::isTargetOfJumpWhoseTargetCanBeChanged(TR::Compilation * comp)
   {
   TR::Block *startBlock = comp->getFlowGraph()->getStart()->asBlock();
   for (auto predEdge = self()->getPredecessors().begin(); predEdge != self()->getPredecessors().end(); ++predEdge)
      {
      TR::Block *predBlock = (*predEdge)->getFrom()->asBlock();
      if (predBlock != startBlock && predBlock->getLastRealTreeTop()->getNode()->getOpCode().isJumpWithMultipleTargets() &&
          predBlock->getLastRealTreeTop()->getNode()->getOpCode().hasBranchChildren())
         return true;
      }
   return false;
   }

TR::Block *
OMR::Block::split(TR::TreeTop * startOfNewBlock, TR::CFG * cfg, bool fixupCommoning, bool copyExceptionSuccessors)
   {
   TR_Structure * structure = cfg->getStructure();
   cfg->setStructure(NULL);
   TR::Compilation * comp = cfg->comp();
   comp->setCurrentBlock(self());
   TR::Node * startNode = startOfNewBlock->getNode();
   TR::Block * block2 = new (self()->trHeapMemory()) TR::Block(TR::TreeTop::create(comp, TR::Node::create(startNode, TR::BBStart)), self()->getExit(), self()->trMemory());
   block2->inheritBlockInfo(self());

   cfg->addNode(block2);


   self()->setExit(TR::TreeTop::create(comp, startOfNewBlock->getPrevTreeTop(), TR::Node::create(startNode, TR::BBEnd)));
   self()->getExit()->join(block2->getEntry());
   self()->getExit()->getNode()->setBlock(self());

   block2->getEntry()->join(startOfNewBlock);

   if (fixupCommoning)
      {
      self()->uncommonNodesBetweenBlocks(comp, block2);
      }

   self()->moveSuccessors(block2);
   cfg->addEdge(self(), block2);
   if (copyExceptionSuccessors)
      cfg->copyExceptionSuccessors(self(), block2);

   if (structure)
      {
      TR_BlockStructure *thisBlockStructure = self()->getStructureOf();
      if (thisBlockStructure)
         {
         TR_BlockStructure *blockStructure2 = new (self()->trHeapMemory()) TR_BlockStructure(comp, block2->getNumber(), block2);
         TR_RegionStructure *parentStructure = thisBlockStructure->getParent()->asRegion();
         TR_StructureSubGraphNode *blockStructureNode2 = new (self()->trHeapMemory()) TR_StructureSubGraphNode(blockStructure2);
         TR_StructureSubGraphNode *subNode;
         TR_RegionStructure::Cursor si(*parentStructure);
         for (subNode = si.getCurrent(); subNode != NULL; subNode = si.getNext())
            {
            if (subNode->getStructure() == thisBlockStructure)
               break;
            }

         parentStructure->addSubNode(blockStructureNode2);

         for (auto nextSucc = subNode->getSuccessors().begin(); nextSucc != subNode->getSuccessors().end(); ++nextSucc)
          (*nextSucc)->setFrom(blockStructureNode2);

         subNode->getSuccessors().clear();
         TR::CFGEdge::createEdge(subNode,  blockStructureNode2, self()->trMemory());

         for (auto nextSucc = subNode->getExceptionSuccessors().begin(); nextSucc != subNode->getExceptionSuccessors().end(); ++nextSucc)
          {
          // Sometimes the catch block is not added to the exc successors
          // by the call to copyExceptionSuccessors above; this can happen if
          // a catch block has the same range as an earlier catch block and is
          // (its catch type) subsumed by the earlier catch block. We should not
          // add the structure edge either in this rare case; exposed by a JCK test
          //
          int32_t nextSuccNum = (*nextSucc)->getTo()->getNumber();
          bool excSuccWasAdded = false;
          for (auto e1 = block2->getExceptionSuccessors().begin(); e1 != block2->getExceptionSuccessors().end(); ++e1)
             {
             if (nextSuccNum == (*e1)->getTo()->getNumber())
               {
               excSuccWasAdded = true;
               break;
               }
             }

          if (excSuccWasAdded)
             {
             if (toStructureSubGraphNode((*nextSucc)->getTo())->getStructure())
                 TR::CFGEdge::createExceptionEdge(blockStructureNode2, (*nextSucc)->getTo(), self()->trMemory());
             else
                parentStructure->addExitEdge(blockStructureNode2, (*nextSucc)->getTo()->getNumber(), true);
             }
          }
         }
      }

   cfg->setStructure(structure);

   return block2;
   }


TR::Block *
OMR::Block::splitBlockAndAddConditional(TR::TreeTop *tree,
                                      TR::TreeTop *compareTree,
                                      TR::CFG *cfg,
                                      bool remainderBlockShouldExtend)
   {
   cfg->setStructure(0);

   TR::Block * remainderBlock = self()->split(tree, cfg, !remainderBlockShouldExtend);
   if (remainderBlockShouldExtend)
      remainderBlock->setIsExtensionOfPreviousBlock();

   self()->append(compareTree);

   // remove the original tree from the remainderBlock
   //
   TR::Node * node = tree->getNode();
   node->removeAllChildren();
   tree->getPrevTreeTop()->join(tree->getNextTreeTop());

   return remainderBlock;
   }


/**
 * createConditionalSideExitBeforeTree converts from
 *    start block
 *      ...
 *      <tree>
 *      ...
 *    end block
 *
 *    to:
 *
 *    start block
 *      ...
 *     if <cond> goto block C (compareTree)
 *    end block
 *    new block B start (remainder)
 *      ... (stuff in original block after <tree>)
 *    new block B end (remainder)
 *
 *    <after next block that cannot fallthrough or at end of cfg>
 *    new block C start (if)
 *      <exitTree>
 *      <returnTree>
 *    new block C end (if)
 *
 * @param this         the block of the original tree to be split up
 * @param tree         the tree identifying the split point. This tree will be removed from its block, compareTree will
 *                     now end this block.
 * @param  compareTree the compare tree to use in constructing the conditional blocks and cfg
 * @param  exitTree    the tree to use on the 'if' branch when constructing the blocks and cfg
 * @param  returnTree  the tree appended to the exitBlock to return
 *                     NOTE: branch destination in node under returnTree MUST be set
 * @param  cfg         the cfg to update with the new information
 * @param  markCold    if the side exit should be marked as cold otherwise inherits frequence from block
 */
TR::Block *
OMR::Block::createConditionalSideExitBeforeTree(TR::TreeTop *tree,
                                              TR::TreeTop *compareTree,
                                              TR::TreeTop *exitTree,
                                              TR::TreeTop *returnTree,
                                              TR::CFG *cfg,
                                              bool markCold)

   {
   auto comp = TR::comp();

   TR::Block *remainderBlock = self()->splitBlockAndAddConditional(tree, compareTree, cfg, true);

   // append the 'exit' block to the end of the method
   // hook the exit tree on to the exit block and insert the return true
   //
   TR::Node *node = tree->getNode();
   TR::Block * exitBlock = TR::Block::createEmptyBlock(node, comp, 0, self());
   cfg->addNode(exitBlock);

   TR::Block *cursorBlock = remainderBlock;
   while (cursorBlock && cursorBlock->canFallThroughToNextBlock())
      cursorBlock = cursorBlock->getNextBlock();

   if (cursorBlock)
      {
      TR::TreeTop *cursorTree = cursorBlock->getExit();
      TR::TreeTop *nextTree = cursorTree->getNextTreeTop();
      cursorTree->join(exitBlock->getEntry());
      exitBlock->getExit()->join(nextTree);
      }
   else
      cfg->findLastTreeTop()->join(exitBlock->getEntry());

   if (markCold)
      {
      exitBlock->setFrequency(UNKNOWN_COLD_BLOCK_COUNT);
      exitBlock->setIsCold();
      }
   else
      {
      exitBlock->setFrequency(remainderBlock->getFrequency());
      }

   exitBlock->append(exitTree);
   exitBlock->append(returnTree);
   compareTree->getNode()->setBranchDestination(exitBlock->getEntry());

   cfg->addEdge(TR::CFGEdge::createEdge(self(),  exitBlock, self()->trMemory()));

   TR::CFGNode *afterExitBlock = cfg->getEnd(); // typically is a return which goes to EXIT
   if (returnTree->getNode()->getOpCode().isBranch())
      afterExitBlock = returnTree->getNode()->getBranchDestination()->getNode()->getBlock();
   cfg->addEdge(TR::CFGEdge::createEdge(exitBlock,  afterExitBlock, self()->trMemory()));

   cfg->copyExceptionSuccessors(self(), exitBlock);

   return remainderBlock;
   }


/**
 * Converts from
 *
 *     start block
 *        ...
 *        <tree>
 *        ...
 *     end block
 *
 *    to:
 *
 *     start block
 *        ...
 *       if <cond> goto block C (rare)
 *     end block
 *     new block B start (else optional)
 *       <elseTree>
 *     new block B end (else optional)
 *     new block D start (remainder)
 *        ... (stuff in original block after <tree>)
 *     new block D end (remainder)
 *
 *     <at end of cfg>
 *     new block C start (if)
 *        <ifTree>
 *        goto block D
 *     new block C end (if)
 *
 *
 *   \param this          the block of the original tree to be split up
 *   \param tree          the tree identifying the split point. This tree will
 *                        be removed from its block, compareTree will now end this block.
 *   \param compareTree   the compare tree to use in constructing the conditional blocks and cfg
 *   \param ifTree        the tree to use on the 'if' branch when constructing the blocks and cfg
 *   \param elseTree      the tree to use on the 'else' branch when constructing the blocks and cfg
 *   \param cfg           the cfg to update with the new information
 */
TR::Block *
OMR::Block::createConditionalBlocksBeforeTree(TR::TreeTop * tree,
                                            TR::TreeTop * compareTree,
                                            TR::TreeTop * ifTree,
                                            TR::TreeTop * elseTree,
                                            TR::CFG * cfg,
                                            bool changeBlockExtensions,
                                            bool markCold)
   {
   auto comp = TR::comp();

   cfg->setStructure(0);

   TR::Block * remainderBlock = self()->split(tree, cfg, true);
   if (changeBlockExtensions)
      remainderBlock->setIsExtensionOfPreviousBlock(false); // not an extension since has 2 predecessors

   self()->append(compareTree);

   // remove the original tree from the remainderBlock
   TR::Node * node = tree->getNode();
   node->removeAllChildren();
   tree->getPrevTreeTop()->join(tree->getNextTreeTop());

   // append the 'if' block to the end of the method
   // hook the if tree on to the if block and add a goto at the end
   // to return to the mainline
   //
   TR::Block * ifBlock = TR::Block::createEmptyBlock(node, comp, 0, self());
   if (markCold)
      {
      ifBlock->setFrequency(UNKNOWN_COLD_BLOCK_COUNT);
      ifBlock->setIsCold();
      }
   else
      {
      ifBlock->setFrequency(remainderBlock->getFrequency());
      }
   cfg->addNode(ifBlock);

   TR::Block *cursorBlock = remainderBlock;
   while (cursorBlock && cursorBlock->canFallThroughToNextBlock())
      {
      cursorBlock = cursorBlock->getNextBlock();
      }

   if (cursorBlock)
      {
      TR::TreeTop *cursorTree = cursorBlock->getExit();
      TR::TreeTop *nextTree = cursorTree->getNextTreeTop();
      cursorTree->join(ifBlock->getEntry());
      ifBlock->getExit()->join(nextTree);
      }
   else
      cfg->findLastTreeTop()->join(ifBlock->getEntry());


   ifBlock->append(ifTree);
   ifBlock->append(TR::TreeTop::create(comp, TR::Node::create(node, TR::Goto, 0, remainderBlock->getEntry())));
   compareTree->getNode()->setBranchDestination(ifBlock->getEntry());

   cfg->addEdge(TR::CFGEdge::createEdge(self(),  ifBlock, self()->trMemory()));
   cfg->addEdge(TR::CFGEdge::createEdge(ifBlock,    remainderBlock, self()->trMemory()));

   cfg->copyExceptionSuccessors(self(), ifBlock);

   // create link in the else tree in the else block
   if (elseTree)
      {
      TR::Block * elseBlock = TR::Block::createEmptyBlock(node, comp, self()->getFrequency(), self());

      elseBlock->append(elseTree);

      self()->getExit()->join(elseBlock->getEntry());
      elseBlock->getExit()->join(remainderBlock->getEntry());

      if (changeBlockExtensions)
         elseBlock->setIsExtensionOfPreviousBlock(true);       // fall-through block is an extension

      cfg->addNode(elseBlock);
      cfg->addEdge(TR::CFGEdge::createEdge(self(),  elseBlock, self()->trMemory()));
      cfg->addEdge(TR::CFGEdge::createEdge(elseBlock,  remainderBlock, self()->trMemory()));
      cfg->copyExceptionSuccessors(self(), elseBlock);
      cfg->removeEdge(self(), remainderBlock);
      }

   return remainderBlock;
   }

OMR::Block::StandardException OMR::Block::_standardExceptions[] =
   {
#define MIN_EXCEPTION_NAME_SIZE 5
   { 5, "Error", CanCatchNew | CanCatchArrayNew },
   { 9, "Exception", CanCatchNullCheck | CanCatchDivCheck | CanCatchBoundCheck |
                     CanCatchArrayStoreCheck | CanCatchArrayNew |
                     CanCatchCheckCast | CanCatchMonitorExit },
   { 9, "Throwable", CanCatchEverything },
   {12, "UnknownError", CanCatchNew | CanCatchArrayNew },
   {13, "InternalError", CanCatchNew | CanCatchArrayNew },
   {16, "OutOfMemoryError", CanCatchNew | CanCatchArrayNew },
   {16, "RuntimeException", CanCatchNullCheck | CanCatchDivCheck |
                            CanCatchBoundCheck | CanCatchArrayStoreCheck |
                            CanCatchArrayNew | CanCatchCheckCast |
                            CanCatchMonitorExit },
   {18, "ClassCastException", CanCatchCheckCast },
   {18, "IllegalAccessError", CanCatchArrayNew },
   {18, "InstantiationError", CanCatchNew },
   {18, "StackOverflowError", CanCatchNew | CanCatchArrayNew },
   {19, "ArithmeticException", CanCatchDivCheck },
   {19, "ArrayStoreException", CanCatchArrayStoreCheck },
   {19, "VirtualMachineError", CanCatchNew | CanCatchArrayNew },
   {20, "NullPointerException", CanCatchNullCheck },
   {25, "IndexOutOfBoundsException", CanCatchBoundCheck },
   {26, "NegativeArraySizeException", CanCatchArrayNew },
   {28, "IllegalMonitorStateException", CanCatchMonitorExit },
   {28, "IncompatibleClassChangeError", CanCatchNew | CanCatchArrayNew },
   {30, "ArrayIndexOutOfBoundsException", CanCatchBoundCheck },
#define MAX_EXCEPTION_NAME_SIZE 30
   {99, "", 0 }
   };

static TR::Node *
findFirstReference(TR::Node * n, TR::Symbol * sym, vcount_t visitCount)
   {
   if (n->getVisitCount() == visitCount)
      return 0;
   n->setVisitCount(visitCount);

   TR::Node * firstReference = 0;
   for (int32_t i = 0; i < n->getNumChildren(); ++i)
      if ((firstReference = findFirstReference(n->getChild(i), sym, visitCount)))
         return firstReference;

   if (n->getOpCode().hasSymbolReference() && n->getSymbol() == sym)
      return n;

   return 0;
   }

TR::Node *
OMR::Block::findFirstReference(TR::Symbol * sym, vcount_t visitCount)
   {
   TR::Node * node = 0;
   for (TR::TreeTop * tt = self()->getFirstRealTreeTop(); tt != self()->getExit(); tt = tt->getNextTreeTop())
      if ((node = ::findFirstReference(tt->getNode(), sym, visitCount)))
         break;
   return node;
   }


void
OMR::Block::collectReferencedAutoSymRefsIn(TR::Compilation *comp, TR_BitVector *referencedAutoSymRefs, vcount_t visitCount)
   {
   if (!self()->getEntry() || !self()->getExit())
      return;

   for (TR::TreeTop * tt = self()->getFirstRealTreeTop(); tt != self()->getExit(); tt = tt->getNextTreeTop())
     self()->collectReferencedAutoSymRefsIn(comp, tt->getNode(), referencedAutoSymRefs, visitCount);
   }

void
OMR::Block::collectReferencedAutoSymRefsIn(TR::Compilation *comp, TR::Node *node, TR_BitVector *referencedAutoSymRefs, vcount_t visitCount)
   {
   if (node->getVisitCount() == visitCount)
      return;

   node->setVisitCount(visitCount);

   if (node->getOpCode().hasSymbolReference() && node->getSymbolReference()->getSymbol()->isAutoOrParm())
      {
      TR::SymbolReference* symRef = node->getSymbolReference();
      if (symRef->getSymbol()->isAutoOrParm() || symRef->getSymbol()->isMethodMetaData())
         referencedAutoSymRefs->set(symRef->getReferenceNumber());
      else if (comp->getOptLevel() > warm)
         {
         TR::SparseBitVector indirectMethodMetaUses(comp->allocator());
         symRef->getUseDefAliases(node->getOpCode().isCallDirect()).getAliases(indirectMethodMetaUses);
         if (!indirectMethodMetaUses.IsZero())
            {
            TR::SparseBitVector::Cursor aliasesCursor(indirectMethodMetaUses);
            for (aliasesCursor.SetToFirstOne(); aliasesCursor.Valid(); aliasesCursor.SetToNextOne())
               {
               int32_t usedSymbolIndex;
               TR::SymbolReference *usedSymRef = comp->getSymRefTab()->getSymRef(aliasesCursor);
               TR::RegisterMappedSymbol *usedSymbol = usedSymRef->getSymbol()->getMethodMetaDataSymbol();
               if (usedSymbol && usedSymbol->getDataType() != TR::NoType)
                  {
                  usedSymbolIndex = usedSymbol->getLiveLocalIndex();
                  referencedAutoSymRefs->set(usedSymbolIndex);
                  }
               }
            }
         }
      }


   for (int32_t i = 0; i < node->getNumChildren(); ++i)
      self()->collectReferencedAutoSymRefsIn(comp, node->getChild(i), referencedAutoSymRefs, visitCount);
   }

TR::Block *
OMR::Block::splitEdge(TR::Block *from, TR::Block *to, TR::Compilation *c, TR::TreeTop **lastTreeTop, bool findOptimalInsertionPoint)
   {

   //traceMsg("Splitting edge (%d,%d)\n", from->getNumber(), to->getNumber());
   TR::Node *exitNode = from->getExit()->getNode();
   TR_Structure *fromContainingLoop = NULL;
   if (from->getStructureOf())
      fromContainingLoop = from->getStructureOf()->getContainingLoop();
   TR_Structure *toContainingLoop = NULL;
   if (to->getStructureOf())
      toContainingLoop = to->getStructureOf()->getContainingLoop();
   if (fromContainingLoop != toContainingLoop)
      {
      while (fromContainingLoop)
         {
         if (fromContainingLoop == toContainingLoop)
            {
            exitNode = to->getEntry()->getNode();
            break;
            }
         fromContainingLoop = fromContainingLoop->getContainingLoop();
         }
      }

   TR::CFG * cfg = c->getFlowGraph();
   TR::Compilation *comp = cfg->comp();
   TR::TreeTop *entryTree = to->getEntry();
   TR::Block * newBlock;
   if (entryTree)
      {
      const bool isExceptionEdge = to->isCatchBlock();
      int32_t freq = from->getEdge(to)->getFrequency();
      newBlock = TR::Block::createEmptyBlock(exitNode, c, freq, from);

      if (from->isCold() || to->isCold())
         {
         newBlock->setFrequency(TR::Block::getMinColdFrequency(from, to));
         newBlock->setIsCold();
         }

      cfg->addNode(newBlock, from->getCommonParentStructureIfExists(to, cfg));
      from->getLastRealTreeTop()->adjustBranchOrSwitchTreeTop(comp, to->getEntry(), newBlock->getEntry());

      // inserting newBlock between insertionExit and insertionEntry treetops
      // by default insertionExit is NULL
      TR::TreeTop *insertionExit = NULL;
      TR::Block *prevBlock = to->getPrevBlock();
      if (prevBlock == from)
         {
         insertionExit = entryTree->getPrevTreeTop();
         }
      else if (findOptimalInsertionPoint && !from->isCold() && !to->isCold())
         {
         while (prevBlock && prevBlock->canFallThroughToNextBlock())
            prevBlock = prevBlock->getPrevBlock();
         if (prevBlock)
            insertionExit = prevBlock->getExit();
         else
            {
            TR::Block *nextBlock = from->getPrevBlock();
            while (nextBlock && nextBlock->canFallThroughToNextBlock())
               nextBlock = nextBlock->getPrevBlock();
            if (nextBlock)
               insertionExit = nextBlock->getExit();
            else
               {
               prevBlock = to->getNextBlock();
               while (prevBlock && prevBlock->canFallThroughToNextBlock())
                  prevBlock = prevBlock->getNextBlock();
               if (prevBlock)
                  insertionExit = prevBlock->getExit();
               else
                  {
                  nextBlock = from->getNextBlock();
                  while (nextBlock && nextBlock->canFallThroughToNextBlock())
                     nextBlock = nextBlock->getNextBlock();
                  if (nextBlock)
                     insertionExit = nextBlock->getExit();
                  }
               }
            }
         }

      if (!insertionExit)
         insertionExit = c->getMethodSymbol()->getLastTreeTop();
      TR::TreeTop *insertionEntry = insertionExit->getNextTreeTop();

      insertionExit->join(newBlock->getEntry());
      newBlock->getExit()->join(insertionEntry);

      if (isExceptionEdge)
         {
         newBlock->setHandlerInfo(
            to->getCatchType(),
            to->getInlineDepth(),
            to->getHandlerIndex(),
            to->getOwningMethod(),
            c);
         TR::ResolvedMethodSymbol *method = c->getMethodSymbol();
         TR::SymbolReferenceTable *srtab = c->getSymRefTab();
         TR::SymbolReference *excSR = srtab->findOrCreateExcpSymbolRef();
         TR::SymbolReference *throwSR = srtab->findOrCreateAThrowSymbolRef(method);
         TR::Node *exc = TR::Node::createLoad(excSR);
         TR::Node *rethrow = TR::Node::createWithSymRef(TR::athrow, 1, 1, exc, throwSR);
         newBlock->append(TR::TreeTop::create(c, rethrow));
         }
      else if (entryTree != insertionEntry)
         {
         TR::TreeTop *gotoTreeTop = TR::TreeTop::create(c, TR::Node::create(from->getExit()->getNode(), TR::Goto, 0, to->getEntry()));
         newBlock->append(gotoTreeTop);
         if (lastTreeTop)
            *lastTreeTop = newBlock->getExit();
         }
      else
         {
         if (to->isExtensionOfPreviousBlock())
            newBlock->setIsExtensionOfPreviousBlock();
         }

      if (isExceptionEdge)
         {
         cfg->addExceptionEdge(newBlock, to);
         cfg->removeEdge(from, to);
         cfg->addExceptionEdge(from, newBlock);
         cfg->addEdge(newBlock, cfg->getEnd());
         }
      else
         {
         cfg->addEdge(from, newBlock);
         cfg->addEdge(newBlock, to);
         cfg->removeEdge(from, to);
         }
      }
   // if entryTree is NULL, then to is exit and from better end in a return or a throw
   // splitting the block at the point of the return or throw is the safest way to do this
   else
      {
      TR::TreeTop *lastTree = from->getLastRealTreeTop();
      TR::ILOpCodes opCode = lastTree->getNode()->getOpCodeValue();
      TR_ASSERT(opCode == TR::athrow || lastTree->getNode()->getOpCode().isReturn(), "edge to EXIT should end in throw or return");

      newBlock = self()->split(lastTree, cfg, true);
      }

   return newBlock;
   }

void
OMR::Block::takeGlRegDeps(TR::Compilation *comp, TR::Node *glRegDeps)
   {
   if (!glRegDeps)
      return;

   TR::Node *duplicateGlRegDeps = glRegDeps->duplicateTree();
   self()->getEntry()->getNode()->setNumChildren(1);
   self()->getEntry()->getNode()->setAndIncChild(0, duplicateGlRegDeps);
   TR::Node *origDuplicateGlRegDeps = duplicateGlRegDeps;
   duplicateGlRegDeps = TR::Node::copy(duplicateGlRegDeps);
   for (int32_t i = origDuplicateGlRegDeps->getNumChildren() - 1; i >= 0; --i)
      {
      TR::Node * dep = origDuplicateGlRegDeps->getChild(i);
      duplicateGlRegDeps->setAndIncChild(i, dep);
      }
   self()->getExit()->getNode()->setNumChildren(1);
   self()->getExit()->getNode()->setChild(0, duplicateGlRegDeps);
   }

TR::TreeTop *
OMR::Block::getExceptingTree()
   {
   TR_ASSERT(self()->getExceptionSuccessors().size() <= 1,"routine only works when block has 0 or 1 (and not %d) exception edges\n",self()->getExceptionSuccessors().size());
   for (TR::TreeTop *tt = self()->getEntry(); tt != self()->getExit(); tt = tt->getNextTreeTop())
      {
      if (tt->getNode()->exceptionsRaised())
         return tt;
      }
   return NULL;
   }

const char *
OMR::Block::getName(TR_Debug * debug)
   {
   if (debug)
      return debug->getName(self());
   else
      return "<unknown block>";
   }

void
OMR::Block::inheritBlockInfo(TR::Block * org, bool inheritFreq)
   {
   self()->setIsCold(org->isCold());
   self()->setIsSuperCold(org->isSuperCold());
   if (inheritFreq)
      self()->setFrequency(org->getFrequency());

   self()->setIsSpecialized(org->isSpecialized());
   }

bool
OMR::Block::isSimpleGenControlBlock()
   {
   if (self()->isGenControlBlock())
      {
      // Block only contain single gen control
      if (self()->getEntry()->getNextRealTreeTop() == self()->getExit()->getPrevRealTreeTop())
         return true;
      }
   return false;
   }

TR::Block *
OMR::Block::startOfExtendedBlock()
   {
   return self()->isExtensionOfPreviousBlock() ?
      self()->getEntry()->getPrevTreeTop()->getNode()->getBlock()->startOfExtendedBlock()
      : self();
   }


bool
OMR::Block::canCatchExceptions(uint32_t flags)
   {
   return _catchBlockExtension
      && ((flags & _catchBlockExtension->_exceptionsCaught) != 0);
   }

void
OMR::Block::setByteCodeIndex(int32_t index, TR::Compilation *comp)
   {
   self()->getEntry()->getNode()->setByteCodeIndex(index);
   }

bool
OMR::Block::endsInGoto()
   {
   TR::TreeTop *lastTT=NULL;
   if (self()->getEntry() != NULL)
      lastTT = self()->getLastRealTreeTop();
   return (lastTT != NULL
           && lastTT->getNode()->getOpCodeValue() == TR::Goto);
   }

bool
OMR::Block::endsInBranch()
   {
   TR::TreeTop * lastTT = NULL;
   TR::Node *    lastNode = NULL;
   if (self()->getEntry() != NULL)
      {
      lastTT = self()->getLastRealTreeTop();
      lastNode = lastTT->getNode();
      }
   return (lastTT != NULL
           && lastNode->getOpCode().isBranch()
           && lastNode->getOpCodeValue() != TR::Goto);
   }

bool
OMR::Block::isEmptyBlock()
   {

   return (self()->getEntry() != NULL && self()->getEntry()->getNextTreeTop() == self()->getExit());
   }


void
OMR::Block::ensureCatchBlockExtensionExists(TR::Compilation *comp)
   {
   if (_catchBlockExtension == NULL)
      {
      _catchBlockExtension = new (comp->trHeapMemory()) TR_CatchBlockExtension();
      }
   }


bool
OMR::Block::hasExceptionPredecessors()
   {
   return !self()->getExceptionPredecessors().empty();
   }



/**
 * Field functions
 */

int32_t
OMR::Block::getNestingDepth()
   {
   if (_pStructureOf)
      return _pStructureOf->getNestingDepth();
   return -1;
   }

TR_Array<TR_GlobalRegister> &
OMR::Block::getGlobalRegisters(TR::Compilation *c)
   {
   if (!_globalRegisters)
      _globalRegisters = new (c->trStackMemory()) TR_Array<TR_GlobalRegister>(c->trMemory(), c->cg()->getNumberOfGlobalRegisters(), true, stackAlloc);
   return *_globalRegisters;
   }

void
OMR::Block::setInstructionBoundaries(uint32_t startPC, uint32_t endPC)
   {
   _instructionBoundaries._startPC = startPC;
   _instructionBoundaries._endPC = endPC;
   }

void
OMR::Block::addExceptionRangeForSnippet(uint32_t startPC, uint32_t endPC)
   {
   _snippetBoundaries.add(new (self()->trHeapMemory()) InstructionBoundaries(startPC, endPC));
   }

void
OMR::Block::setHandlerInfo(uint32_t c, uint8_t d, uint16_t i, TR_ResolvedMethod * m, TR::Compilation *comp)
   {
   self()->ensureCatchBlockExtensionExists(comp);
   TR_CatchBlockExtension *cbe = _catchBlockExtension;
   cbe->_catchType = c;
   cbe->_inlineDepth = d;
   cbe->_handlerIndex = i;
   cbe->_exceptionsCaught = CanCatchEverything;
   cbe->_owningMethod = m;
   cbe->_byteCodeInfo = _pEntry->getNode()->getByteCodeInfo();

   if (c != 0)
      {
      uint32_t length;
      char * name = m->getClassNameFromConstantPool(c, length);
      self()->setExceptionClassName(name, length, comp);
      }
   }

void
OMR::Block::setHandlerInfoWithOutBCInfo(uint32_t c, uint8_t d, uint16_t i, TR_ResolvedMethod * m, TR::Compilation *comp)
   {
   self()->ensureCatchBlockExtensionExists(comp);
   TR_CatchBlockExtension *cbe = _catchBlockExtension;
   cbe->_catchType = c;
   cbe->_inlineDepth = d;
   cbe->_handlerIndex = i;
   if (c == CanCatchOSR)
     cbe->_exceptionsCaught = CanCatchEverything;
   cbe->_owningMethod = m;
   }

bool
OMR::Block::isCatchBlock()
   {
   return !self()->getExceptionPredecessors().empty();
   }

uint32_t
OMR::Block::getCatchType()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_catchType;
   }

uint8_t
OMR::Block::getInlineDepth()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_inlineDepth;
   }

uint16_t
OMR::Block::getHandlerIndex()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_handlerIndex;
   }

TR_ByteCodeInfo
OMR::Block::getByteCodeInfo()
   {
   TR_ASSERT(_catchBlockExtension, "Catch block extension does not exist");
   return _catchBlockExtension->_byteCodeInfo;
   }

TR_OpaqueClassBlock *
OMR::Block::getExceptionClass()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_exceptionClass;
   }

char *
OMR::Block::getExceptionClassNameChars()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_exceptionClassNameChars;
   }

int32_t
OMR::Block::getExceptionClassNameLength()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_exceptionClassNameLength;
   }

void
OMR::Block::setExceptionClassName(char *name, int32_t length, TR::Compilation *comp)
   {
   self()->ensureCatchBlockExtensionExists(comp);
   _catchBlockExtension->_exceptionClassNameChars  = name;
   _catchBlockExtension->_exceptionClassNameLength = length;

   if (name == NULL)
      {
      _catchBlockExtension->_exceptionsCaught = CanCatchEverything;
      _catchBlockExtension->_exceptionClass = 0;
      return;
      }

   _catchBlockExtension->_exceptionClass =
      self()->getOwningMethod()->fe()->getClassFromSignature(name, length, self()->getOwningMethod());

   // Set up flags for which standard exceptions can be caught by this block
   //
   _catchBlockExtension->_exceptionsCaught = CanCatchUserThrows | CanCatchResolveCheck;

   if (length < 10+MIN_EXCEPTION_NAME_SIZE ||
       length > 10+MAX_EXCEPTION_NAME_SIZE ||
       strncmp(name, "java/lang/", 10))
      return;

   name   += 10;
   length -= 10;

   for (int32_t i = 0; ; ++i)
      {
      StandardException &excp = _standardExceptions[i];
      if (excp.length > length)
         break;
      if (excp.length == length && !strncmp(name, excp.name, length))
         {
         _catchBlockExtension->_exceptionsCaught |= excp.exceptions;
         break;
         }
      }
   }

TR_ResolvedMethod *
OMR::Block::getOwningMethod()
   {
   TR_ASSERT(_catchBlockExtension, "catch block extension does not exist");
   return _catchBlockExtension->_owningMethod;
   }

int32_t
OMR::Block::getNormalizedFrequency(TR::CFG * cfg)
   {
   if (self()->getFrequency() < 0)
      return (MAX_WARM_BLOCK_COUNT * 100) / (MAX_BLOCK_COUNT+MAX_COLD_BLOCK_COUNT);
   return (self()->getFrequency() * 100) / (MAX_BLOCK_COUNT+MAX_COLD_BLOCK_COUNT);
   }

int32_t
OMR::Block::getGlobalNormalizedFrequency(TR::CFG * cfg)
   {
   int32_t frequency = self()->getNormalizedFrequency(cfg);

   TR_Hotness hotness = cfg->comp()->getMethodHotness();
   if (hotness >= scorching)
      return frequency * 100;
   if (hotness >= hot)
      return frequency * 10;
   return frequency;
   }

/**
 * Field functions end
 */

/**
 * Flag functions
 */

void
OMR::Block::setIsExtensionOfPreviousBlock(bool b)
   {
   _flags.set(_isExtensionOfPreviousBlock, b);
   if (!b && TR::comp()->getOptimizer())
      {
      TR::comp()->getOptimizer()->setCachedExtendedBBInfoValid(false);
      }
   }

bool
OMR::Block::isExtensionOfPreviousBlock()
   {
   return _flags.testAny(_isExtensionOfPreviousBlock);
   }

void
OMR::Block::setIsSuperCold(bool b)
   {
   _flags.set(_isSuperCold, b);
   if (b)
      {
      self()->setIsCold();
      self()->setFrequency(0);
      }
   }

bool
OMR::Block::isSuperCold()
   {
   return _flags.testAny(_isSuperCold);
   }

/**
 * Flag functions end
 */

TR::Block *
TR_BlockCloner::cloneBlocks(TR::Block * from, TR::Block * lastBlock)
   {
   TR_LinkHeadAndTail<BlockMapper> bMap;
   bMap.set(0, 0);

   // The block mappings (including the BBStart treetops) have to be created
   // before any branch nodes are cloned so that mappings can be used to fixup
   // the destination of the cloned branch.
   // However, the nodes must all be created at the same time so that nodes can
   // be shared properly.
   // To do this, trees are created in two passes. In the first pass the cloned
   // blocks are created along with the BBStart and BBEnd treetops (but not the
   // nodes). In the second pass the BBStart and BBEnd nodes are created, and
   // the treetops and nodes for the rest of the block.
   //
   TR::Compilation *comp = _cfg->comp();
   for (; from; from = from->getNextBlock())
      {
      comp->setCurrentBlock(from);
      TR::Block * to =
         new (from->trHeapMemory()) TR::Block(*from, TR::TreeTop::create(comp, NULL), TR::TreeTop::create(comp, NULL));
      to->getEntry()->join(to->getExit());

      if (bMap.getLast())
         bMap.getLast()->_to->getExit()->join(to->getEntry());
      bMap.append(new (comp->trStackMemory()) BlockMapper(from, to));
      if (from == lastBlock)
         break;
      }
   return doBlockClone(&bMap);
   }

TR::Block *
TR_BlockCloner::cloneBlocks(TR_LinkHeadAndTail<BlockMapper>* bMap)
   {
   TR::Block * toReturn = doBlockClone(bMap);

   // for a path of blocks we're cloning that doesn't follow the fall-through path, the branches will be set to fall through node (incorrectly) by cloneNode.  This code patches up the branches so they point to the right target (the old fall through node)
   // we don't mess with the last node as the connection is handled by whoever called us
   //
   for (BlockMapper* itr = bMap->getFirst(); itr->getNext(); itr = itr->getNext())
      {
      TR::Node* lastRealNode = itr->_to->getExit()->getPrevRealTreeTop()->getNode();
      TR::TreeTop* nextTree = itr->_from->getExit()->getNextTreeTop();
      //if we are in the middle of the clone check and see if we took a branch at an if
      //if branch was taken reverse the branch condition so we fall through to the next block as expected
      //
      if (
          lastRealNode->getOpCode().isIf() &&
          lastRealNode->getBranchDestination()->getNode()->getBlock()->getNumber() == itr->_to->getNextBlock()->getNumber()
         )
         {
         TR::TreeTop *newDest = NULL;
         if (_cloneBranchesExactly)
            newDest = itr->_from->getExit()->getNextTreeTop();
         else
            newDest = getToBlock(itr->_from->getExit()->getNextTreeTop()->getNode()->getBlock())->getEntry();
         lastRealNode->reverseBranch(newDest);
         }
      }

   return toReturn;
   }

TR::Block *
TR_BlockCloner::getToBlock(TR::Block * from)
   {
   for (BlockMapper * m = _blockMappings.getFirst(); m; m = m->getNext())
      if (m->_from == from)
         return m->_to;
   return from;
   }

TR::Block *
TR_BlockCloner::doBlockClone(TR_LinkHeadAndTail<BlockMapper>* bMap)
   {
   _blockMappings = *bMap;
   TR::Compilation *comp = _cfg->comp();
   BlockMapper * m;
   for (m = _blockMappings.getFirst(); m; m = m->getNext())
      {
      TR::TreeTop * fromTT = m->_from->getEntry();

      // Start with new node mappings if this is the start of a new extended
      // basic block.
      //
      if (!fromTT->getNode()->getBlock()->isExtensionOfPreviousBlock())
         _nodeMappings.clear();

      // Create the BBStart node
      //
      m->_to->getEntry()->setNode(cloneNode(fromTT->getNode()));
      m->_to->getEntry()->getNode()->setBlock(m->_to);

      // Create the trees for the body of the block
      //
      for (fromTT = fromTT->getNextTreeTop(); fromTT != m->_from->getExit(); fromTT = fromTT->getNextTreeTop())
         {
         TR::TreeTop * toTT = TR::TreeTop::create(comp, cloneNode(fromTT->getNode()));
         m->_to->append(toTT);
         }

      // Create the BBEnd node
      //
      m->_to->getExit()->setNode(cloneNode(fromTT->getNode()));
      m->_to->getExit()->getNode()->setBlock(m->_to);

      comp->setCurrentBlock(m->_from);

      }

   // Create CFG edges for the cloned blocks
   //
   // Some Extra remarks about this code.  This cfg code looks insufficient to really handle the case when you are not cloning along a
   // fall through path, and you are trying to preserve branches exactly (ie _cloneBranchesExactly is set to true
   // Luckily, so far no code has ever not done that.  Most code that clonesBranchesExactly clones a single block, and the few cases that
   // don't appear to be cloning along a fall through path
   //
   for (m = _blockMappings.getFirst(); m; m = m->getNext())
      {
      _cfg->addNode(m->_to);
      if (comp->ilGenTrace())
         dumpOptDetails(comp,"BLOCK CLONER: Newly created block_%d is a clone of original block_%d\n", m->_to->getNumber(), m->_from->getNumber());
      if (_cloneSuccessorsOfLastBlock ||
          m != _blockMappings.getLast())
         {
         for (auto e = m->_from->getSuccessors().begin(); e != m->_from->getSuccessors().end(); ++e)
            _cfg->addEdge(m->_to, getToBlock(toBlock((*e)->getTo())));

         for (auto e = m->_from->getExceptionSuccessors().begin(); e != m->_from->getExceptionSuccessors().end(); ++e)
            _cfg->addExceptionEdge(m->_to, getToBlock(toBlock((*e)->getTo())));

         for (auto e = m->_from->getExceptionPredecessors().begin(); e != m->_from->getExceptionPredecessors().end(); ++e)
            _cfg->addExceptionEdge(toBlock((*e)->getFrom()), m->_to);
         }
      }

   _lastToBlock = _blockMappings.getLast()->_to;

   return _blockMappings.getFirst()->_to;
   }

TR::Node *
TR_BlockCloner::cloneNode(TR::Node * from)
   {
   TR::Node * to;
   if (from->getReferenceCount() > 1 && (to = _nodeMappings.getTo(from)))
      return to;

   int32_t n = from->getNumChildren();
   to = TR::Node::copy(from);

   if (from->getOpCode().getOpCodeValue() == TR::allocationFence)
      {
      if (from->getAllocation())
         {
         TR::Node *dupAllocNode = _nodeMappings.getTo(from->getAllocation());
         if (dupAllocNode)
            to->setAllocation(dupAllocNode);
         }
      }

   if (from->getOpCode().isBranch())
      {
      if (_cloneBranchesExactly)
         to->setBranchDestination(from->getBranchDestination());
      else
         to->setBranchDestination(getToBlock(from->getBranchDestination()->getNode()->getBlock())->getEntry());
      }

   for (int32_t i = 0; i < n; ++i)
      to->setChild(i, cloneNode(from->getChild(i)));

   if (from->getReferenceCount() > 1)
      _nodeMappings.add(from, to, _cfg->comp()->trMemory());

   return to;
   }

TR::Block *
TR_ExtendedBlockSuccessorIterator::getFirst()
   {
   if (_firstBlock == _cfg->getEnd())
      return 0;

   setCurrentBlock(_firstBlock);
   _iterator = _list->begin();
   TR::Block * b = toBlock(_list->front()->getTo());
   return b == _nextBlockInExtendedBlock ? getNext() : b;
   }

TR::Block *
TR_ExtendedBlockSuccessorIterator::getNext()
   {
   TR::CFGEdge* edge;
   if ((_iterator == _list->end()) || (++_iterator == _list->end()))
      {
      if (!_nextBlockInExtendedBlock)
         return 0;
      setCurrentBlock(_nextBlockInExtendedBlock);
      edge = _list->empty() ? NULL : _list->front();
      _iterator = _list->begin();
      }
   else
      edge = *_iterator;
   TR::Block * b = edge ? toBlock(edge->getTo()) : NULL;
   return b == _nextBlockInExtendedBlock ? getNext() : b;
   }

void
TR_ExtendedBlockSuccessorIterator::setCurrentBlock(TR::Block * b)
   {
   _list = &b->getSuccessors();
   _iterator = _list->begin();
   TR::Block * nextBlock = b->getNextBlock();
   _nextBlockInExtendedBlock = nextBlock && nextBlock->isExtensionOfPreviousBlock() ? nextBlock : 0;
   }
