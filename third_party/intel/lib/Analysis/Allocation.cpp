#include "intel/include/Analysis/Allocation.h"

#include <algorithm>
#include <limits>
#include <numeric>

#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Analysis/Liveness.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Support/LLVM.h"
#include "triton/Analysis/Alias.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "llvm/ADT/SmallVector.h"

#include "intel/include/Analysis/Utility.h"

using ::mlir::triton::gpu::AMDMfmaEncodingAttr;
using ::mlir::triton::gpu::BlockedEncodingAttr;
using ::mlir::triton::gpu::DotOperandEncodingAttr;
using ::mlir::triton::gpu::getContigPerThread;
using ::mlir::triton::gpu::getOrder;
using ::mlir::triton::gpu::getShapePerCTA;
using ::mlir::triton::gpu::getShapePerCTATile;
using ::mlir::triton::gpu::getSizePerThread;
using ::mlir::triton::gpu::getUniqueContigPerThread;
using ::mlir::triton::gpu::NvidiaMmaEncodingAttr;
using ::mlir::triton::gpu::SharedEncodingAttr;
using ::mlir::triton::gpu::SliceEncodingAttr;

namespace mlir {

//===----------------------------------------------------------------------===//
// Shared Memory Allocation Analysis
//===----------------------------------------------------------------------===//
namespace triton::intel {

// Bitwidth of pointers
constexpr int kPtrBitWidth = 64;

static std::pair<SmallVector<unsigned>, SmallVector<unsigned>>
getCvtOrder(Attribute srcLayout, Attribute dstLayout) {
  auto srcMmaLayout = mlir::dyn_cast<NvidiaMmaEncodingAttr>(srcLayout);
  auto srcDotLayout = mlir::dyn_cast<DotOperandEncodingAttr>(srcLayout);
  auto dstMmaLayout = mlir::dyn_cast<NvidiaMmaEncodingAttr>(dstLayout);
  auto dstDotLayout = mlir::dyn_cast<DotOperandEncodingAttr>(dstLayout);

  assert(!(srcMmaLayout && dstMmaLayout && !srcMmaLayout.isAmpere() &&
           !srcMmaLayout.isHopper()) &&
         "mma -> mma layout conversion is only supported on Ampere");

  // mma or dot layout does not have an order, so the order depends on the
  // layout of the other operand.
  auto inOrd = (srcMmaLayout || srcDotLayout) ? getOrder(dstLayout)
                                              : getOrder(srcLayout);
  auto outOrd = (dstMmaLayout || dstDotLayout) ? getOrder(srcLayout)
                                               : getOrder(dstLayout);

  return {inOrd, outOrd};
}

static SmallVector<unsigned> getRepShapeForCvt(RankedTensorType srcTy,
                                               RankedTensorType dstTy) {
  Attribute srcLayout = srcTy.getEncoding();
  Attribute dstLayout = dstTy.getEncoding();

  if (!cvtNeedsSharedMemory(srcTy, dstTy)) {
    return {};
  }

  if (shouldUseDistSmem(srcLayout, dstLayout)) {
    // TODO: padding to avoid bank conflicts
    return convertType<unsigned, int64_t>(getShapePerCTA(srcTy));
  }

  assert(srcLayout && dstLayout && "Unexpected layout in getRepShapeForCvt()");

  auto srcShapePerCTA = getShapePerCTA(srcTy);
  auto dstShapePerCTA = getShapePerCTA(dstTy);
  auto srcShapePerCTATile = getShapePerCTATile(srcLayout, srcTy.getShape());
  auto dstShapePerCTATile = getShapePerCTATile(dstLayout, dstTy.getShape());

  unsigned rank = dstTy.getRank();
  SmallVector<unsigned> repShape(rank);
  for (unsigned d = 0; d < rank; ++d) {
    repShape[d] =
        std::max(std::min<unsigned>(srcShapePerCTA[d], srcShapePerCTATile[d]),
                 std::min<unsigned>(dstShapePerCTA[d], dstShapePerCTATile[d]));
  }
  return repShape;
}

// Both `atomic_cas` and `atomic_rmw need a single scratch element if returning
// a scalar value because Triton's block-based programming model ensures that
// all threads in each block see the same return value, even those threads that
// do not participate in the atomic operation
static SmallVector<unsigned> getRepShapeForAtomic(Value result) {
  SmallVector<unsigned> smemShape;
  if (atomicNeedsSharedMemory(result)) {
    smemShape.push_back(1);
  }
  return smemShape;
}

ScratchConfig getScratchConfigForCvt(RankedTensorType srcTy,
                                     RankedTensorType dstTy) {
  if (gpu::intel::cvtIsSubGroupShuffle(srcTy, dstTy)) {
    // Conversions that can be implemented as sub-group shuffles do not need
    // scratch memory.
    return ScratchConfig({}, {});
  }

  if (gpu::intel::cvtIsSubGroupTranspose(srcTy, dstTy)) {
    // Conversions that can be implemented as sub-group transposes store the
    // whole tensor in shared memory and read it afterwards.
    auto srcEncoding = cast<gpu::DistributedEncodingTrait>(srcTy.getEncoding());
    unsigned threadsPerWarp = product(srcEncoding.getThreadsPerWarp());
    unsigned warpsPerCTA = product(srcEncoding.getWarpsPerCTA());
    unsigned remaining = product(srcTy.getShape()) /
                         (threadsPerWarp * threadsPerWarp * warpsPerCTA);
    SmallVector<unsigned> repShape{threadsPerWarp, threadsPerWarp, remaining,
                                   warpsPerCTA};
    return ScratchConfig(repShape, repShape,
                         /*inVec=*/1, /*outVec=*/threadsPerWarp);
  }

  // Initialize vector sizes and stride
  auto repShape = getRepShapeForCvt(srcTy, dstTy);
  if (repShape.empty())
    return ScratchConfig({}, {});
  ScratchConfig scratchConfig(repShape, repShape);
  auto rank = repShape.size();
  Attribute srcLayout = srcTy.getEncoding();
  Attribute dstLayout = dstTy.getEncoding();

  assert(cvtNeedsSharedMemory(srcTy, dstTy));

  // FIXME This is NOT entirely correct
  // This should be getElemOrder, but we don't have such a method
  // TODO Implement getElemOrder and make sure it's consistent with
  // getContigPerThread
  auto inOrd = gpu::getThreadOrder(srcLayout);
  auto outOrd = gpu::getThreadOrder(dstLayout);
  scratchConfig.order = outOrd;

  unsigned srcContigPerThread =
      getUniqueContigPerThread(srcLayout, srcTy.getShape())[inOrd[0]];
  unsigned dstContigPerThread =
      getUniqueContigPerThread(dstLayout, dstTy.getShape())[outOrd[0]];
  // TODO: Fix the legacy issue that ourOrd[0] == 0 always means
  //       that we cannot do vectorization.
  unsigned innerDim = rank - 1;
  scratchConfig.inVec = outOrd[0] != innerDim  ? 1
                        : inOrd[0] != innerDim ? 1
                                               : srcContigPerThread;
  scratchConfig.outVec = outOrd[0] != innerDim ? 1 : dstContigPerThread;

  if (auto mma = mlir::dyn_cast<NvidiaMmaEncodingAttr>(srcLayout)) {
    if (mma.getVersionMajor() == 1) {
      // For conversions to MmaV1 (Nvidia V100), this inVec is hardcoded in the
      // codegen.
      scratchConfig.inVec = srcContigPerThread;
    } else if (mlir::isa<BlockedEncodingAttr>(dstLayout)) {
      // when storing from mma layout and loading in blocked layout vectorizing
      // the load back gives better performance even if there is a
      // transposition.
      scratchConfig.outVec = dstContigPerThread;
    }
  }

  // No padding is required if the tensor is 1-D, or if all dimensions except
  // the first accessed dimension have a size of 1.
  if (rank <= 1 || product(repShape) == repShape[outOrd[0]])
    return scratchConfig;

  auto paddedSize = std::max(scratchConfig.inVec, scratchConfig.outVec);
  scratchConfig.paddedRepShape[outOrd[0]] += paddedSize;
  return scratchConfig;
}

class AllocationAnalysis {
public:
  AllocationAnalysis(Operation *operation,
                     Allocation::FuncAllocMapT *funcAllocMap,
                     Allocation *allocation)
      : operation(operation), funcAllocMap(funcAllocMap),
        allocation(allocation) {
    run();
  }

private:
  using BufferT = Allocation::BufferT;

  /// Value -> Liveness Range
  /// Use MapVector to ensure determinism.
  using BufferRangeMapT = llvm::MapVector<BufferT *, Interval<size_t>>;
  /// Nodes -> Nodes
  using GraphT = DenseMap<BufferT *, DenseSet<BufferT *>>;

  void run() {
    getValuesAndSizes();
    resolveLiveness();
    computeOffsets();
  }

  /// Initializes explicitly defined shared memory values for a given operation.
  void getExplicitValueSize(Operation *op) {
    for (Value result : op->getResults()) {
      auto alloc = result.getDefiningOp<triton::gpu::LocalAllocOp>();
      if (alloc && alloc.isSharedMemoryAlloc()) {
        // Bytes could be a different value once we support padding or other
        // allocation policies.
        auto allocType = alloc.getType();
        auto shapePerCTA = triton::gpu::getShapePerCTA(allocType);
        auto bytes = product<int64_t>(shapePerCTA) *
                     allocType.getElementTypeBitWidth() / 8;

        auto alignment = alloc.getAlignmentOrDefault();
        allocation->addBuffer<BufferT::BufferKind::Explicit>(result, bytes,
                                                             alignment);
      }
    }
  }

  template <BufferT::BufferKind T>
  void maybeAddScratchBuffer(Operation *op, unsigned bytes,
                             unsigned alignment) {
    if (bytes > 0)
      allocation->addBuffer<T>(op, bytes, alignment);
  }

  template <BufferT::BufferKind T>
  void maybeAddScratchBuffer(Operation *op, unsigned bytes) {
    if (bytes > 0)
      allocation->addBuffer<T>(op, bytes);
  }

  /// Initializes temporary shared memory for a given operation.
  void getScratchValueSize(Operation *op) {
    const size_t scratchAlignment = 128;
    if (auto reduceOp = dyn_cast<triton::ReduceOp>(op)) {
      ReduceOpHelper helper(reduceOp);
      unsigned bytes = helper.getScratchSizeInBytes();
      maybeAddScratchBuffer<BufferT::BufferKind::Scratch>(op, bytes,
                                                          scratchAlignment);
    } else if (auto scanOp = dyn_cast<triton::ScanOp>(op)) {
      ScanLoweringHelper helper(scanOp);
      unsigned bytes = helper.getScratchSizeInBytes();
      maybeAddScratchBuffer<BufferT::BufferKind::Scratch>(op, bytes,
                                                          scratchAlignment);
    } else if (auto histogram = dyn_cast<triton::HistogramOp>(op)) {
      auto dstTy = histogram.getType();
      int threadsPerWarp = triton::gpu::TritonGPUDialect::getThreadsPerWarp(
          op->getParentOfType<ModuleOp>());
      auto bytes = std::max<int>(dstTy.getNumElements(), threadsPerWarp) *
                   std::max<int>(8, dstTy.getElementTypeBitWidth()) / 8;
      maybeAddScratchBuffer<BufferT::BufferKind::Scratch>(op, bytes,
                                                          scratchAlignment);
    } else if (auto cvtLayout = dyn_cast<triton::gpu::ConvertLayoutOp>(op)) {
      auto srcTy = cvtLayout.getSrc().getType();
      auto dstTy = cvtLayout.getType();
      auto srcEncoding = srcTy.getEncoding();
      auto dstEncoding = dstTy.getEncoding();
      if (mlir::isa<SharedEncodingAttr>(srcEncoding) ||
          mlir::isa<SharedEncodingAttr>(dstEncoding)) {
        // Conversions from/to shared memory do not need scratch memory.
        return;
      }
      // ConvertLayoutOp with both input/output non-shared_layout
      // TODO: Besides of implementing ConvertLayoutOp via shared memory, it's
      //       also possible to realize it with other approaches in restricted
      //       conditions, such as warp-shuffle
      auto scratchConfig = getScratchConfigForCvt(srcTy, dstTy);
      auto elems = getNumScratchElements(scratchConfig.paddedRepShape);
      auto bytes =
          isa<triton::PointerType>(srcTy.getElementType())
              ? elems * kPtrBitWidth / 8
              : elems * std::max<int>(8, srcTy.getElementTypeBitWidth()) / 8;
      maybeAddScratchBuffer<BufferT::BufferKind::Scratch>(op, bytes,
                                                          scratchAlignment);
    } else if (isa<triton::AtomicRMWOp, triton::AtomicCASOp>(op)) {
      auto value = op->getOperand(0);
      // only scalar requires scratch memory
      // make it explicit for readability
      if (dyn_cast<RankedTensorType>(value.getType())) {
        // nothing to do
      } else {
        auto smemShape = getRepShapeForAtomic(op->getResult(0));
        auto elems = getNumScratchElements(smemShape);
        auto elemTy =
            cast<triton::PointerType>(value.getType()).getPointeeType();
        auto bytes =
            isa<triton::PointerType>(elemTy)
                ? elems * kPtrBitWidth / 8
                : elems * std::max<int>(8, elemTy.getIntOrFloatBitWidth()) / 8;
        maybeAddScratchBuffer<BufferT::BufferKind::Scratch>(op, bytes,
                                                            scratchAlignment);
      }
    } else if (auto callOp = dyn_cast<CallOpInterface>(op)) {
      auto callable = callOp.resolveCallable();
      auto funcOp = dyn_cast<FunctionOpInterface>(callable);
      auto *funcAlloc = &(*funcAllocMap)[funcOp];
      auto bytes = funcAlloc->getSharedMemorySize();
      maybeAddScratchBuffer<BufferT::BufferKind::Virtual>(op, bytes,
                                                          scratchAlignment);
    } else if (auto createTensormap =
                   dyn_cast<ExperimentalTensormapCreateOp>(op)) {
      constexpr int32_t kTMASize = 128;
      constexpr int32_t kTMAAlign = 128;
      maybeAddScratchBuffer<BufferT::BufferKind::Scratch>(op, kTMASize,
                                                          kTMAAlign);
    }
  }

  void getValueAlias(Value value, SharedMemoryAliasAnalysis &analysis) {
    dataflow::Lattice<AliasInfo> *latticeElement =
        analysis.getLatticeElement(value);
    if (latticeElement) {
      AliasInfo &info = latticeElement->getValue();
      if (!info.getAllocs().empty()) {
        for (auto alloc : info.getAllocs()) {
          allocation->addAlias(value, alloc);
        }
      }
    }
  }

  /// Extract all shared memory values and their sizes
  void getValuesAndSizes() {
    // Get the alloc values
    operation->walk<WalkOrder::PreOrder>([&](Operation *op) {
      getExplicitValueSize(op);
      getScratchValueSize(op);
    });
    // Get the alias values
    std::unique_ptr<DataFlowSolver> solver = createDataFlowSolver();
    SharedMemoryAliasAnalysis *aliasAnalysis =
        solver->load<SharedMemoryAliasAnalysis>();
    if (failed(solver->initializeAndRun(operation))) {
      // TODO: return error instead of bailing out..
      llvm_unreachable("failed to run SharedMemoryAliasAnalysis");
    }
    operation->walk<WalkOrder::PreOrder>([&](Operation *op) {
      for (auto operand : op->getOperands()) {
        getValueAlias(operand, *aliasAnalysis);
      }
      for (auto value : op->getResults()) {
        getValueAlias(value, *aliasAnalysis);
      }
    });
  }

  /// Computes the liveness range of the allocated value.
  /// Each buffer is allocated only once.
  void resolveExplicitBufferLiveness(
      function_ref<Interval<size_t>(Value value)> getLiveness) {
    for (auto valueBufferIter : allocation->getValueBuffer()) {
      auto value = valueBufferIter.first;
      auto *buffer = valueBufferIter.second;
      bufferRange[buffer] = getLiveness(value);
    }
  }

  /// Extends the liveness range by unionizing the liveness range of the aliased
  /// values because each allocated buffer could be an alias of others, if block
  /// arguments are involved.
  void resolveAliasBufferLiveness(
      function_ref<Interval<size_t>(Value value)> getLiveness) {
    for (auto aliasBufferIter : allocation->getAliasBuffer()) {
      auto value = aliasBufferIter.first;
      auto buffers = aliasBufferIter.second;
      auto range = getLiveness(value);
      for (auto *buffer : buffers) {
        auto minId = range.start();
        auto maxId = range.end();
        if (bufferRange.count(buffer)) {
          // Extend the allocated buffer's range
          minId = std::min(minId, bufferRange[buffer].start());
          maxId = std::max(maxId, bufferRange[buffer].end());
        }
        bufferRange[buffer] = Interval(minId, maxId);
      }
    }
  }

  /// Computes the liveness range of scratched buffers.
  /// Some operations may have a temporary buffer that is not explicitly
  /// allocated, but is used to store intermediate results.
  void resolveScratchBufferLiveness(
      const DenseMap<Operation *, size_t> &operationId) {
    // Analyze liveness of scratch buffers and virtual buffers.
    auto processScratchMemory = [&](const auto &container) {
      for (auto opScratchIter : container) {
        // Any scratch memory's live range is the current operation's live
        // range.
        auto *op = opScratchIter.first;
        auto *buffer = opScratchIter.second;
        bufferRange.insert({buffer, Interval(operationId.lookup(op),
                                             operationId.lookup(op) + 1)});
      }
    };
    processScratchMemory(allocation->getOpScratch());
    processScratchMemory(allocation->getOpVirtual());
  }

  /// Resolves liveness of all values involved under the root operation.
  void resolveLiveness() {
    // Assign an ID to each operation using post-order traversal.
    // To achieve the correct liveness range, the parent operation's ID
    // should be greater than each of its child operation's ID .
    // Example:
    //     ...
    //     %5 = triton.convert_layout %4
    //     %6 = scf.for ... iter_args(%arg0 = %0) -> (i32) {
    //       %2 = triton.convert_layout %5
    //       ...
    //       scf.yield %arg0
    //     }
    // For example, %5 is defined in the parent region and used in
    // the child region, and is not passed as a block argument.
    // %6 should should have an ID greater than its child operations,
    // otherwise %5 liveness range ends before the child operation's liveness
    // range ends.
    DenseMap<Operation *, size_t> operationId;
    operation->walk<WalkOrder::PostOrder>(
        [&](Operation *op) { operationId[op] = operationId.size(); });

    // Analyze liveness of explicit buffers
    Liveness liveness(operation);
    auto getValueLivenessRange = [&](Value value) {
      auto liveOperations = liveness.resolveLiveness(value);
      auto minId = std::numeric_limits<size_t>::max();
      auto maxId = std::numeric_limits<size_t>::min();
      std::for_each(liveOperations.begin(), liveOperations.end(),
                    [&](Operation *liveOp) {
                      if (operationId[liveOp] < minId) {
                        minId = operationId[liveOp];
                      }
                      if ((operationId[liveOp] + 1) > maxId) {
                        maxId = operationId[liveOp] + 1;
                      }
                    });
      return Interval(minId, maxId);
    };

    resolveExplicitBufferLiveness(getValueLivenessRange);
    resolveAliasBufferLiveness(getValueLivenessRange);
    resolveScratchBufferLiveness(operationId);
  }

  /// Computes the shared memory offsets for all related values.
  /// Paper: Algorithms for Compile-Time Memory Optimization
  /// (https://dl.acm.org/doi/pdf/10.5555/314500.315082)
  void computeOffsets() {
    SmallVector<BufferT *> buffers;
    for (auto bufferIter : bufferRange) {
      buffers.emplace_back(bufferIter.first);
    }

    calculateStarts(buffers);

    // NOTE: The original paper doesn't consider interference between
    // the bumped ranges. Buffers that previously do not interfere with
    // could interfere after offset bumping if their liveness ranges overlap.
    // Therefore, we rerun the interference graph algorithm after bumping so
    // that we regroup the buffers and color them again. Since we always
    // increase the buffer offset and keep reducing conflicts, we will
    // eventually reach a fixed point.
    GraphT interference;
    buildInterferenceGraph(buffers, interference);
    do {
      allocate(buffers, interference);
      buildInterferenceGraph(buffers, interference);
    } while (!interference.empty());
  }

  /// Computes the initial shared memory offsets.
  void calculateStarts(const SmallVector<BufferT *> &buffers) {
    //  v = values in shared memory
    //  t = triplet of (size, start, end)
    //  shared memory space
    //  -
    //  |         *******t4
    //  | /|\ v2 inserts t4, t5, and t6
    //  |  |
    //  | ******t5         ************t6
    //  | ^^^^^v2^^^^^^
    //  |  |      *********************t2
    //  | \|/ v2 erases t1
    //  | ******t1 ^^^^^^^^^v1^^^^^^^^^ ************t3
    //  |---------------------------------------------| liveness range
    //    1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 ...
    // If the available triple's range is less than a given buffer range,
    // we won't know if there has been an overlap without using graph coloring.
    // Start -> Liveness Range
    using TripleMapT = std::multimap<size_t, Interval<size_t>>;
    TripleMapT tripleMap;
    tripleMap.insert(std::make_pair(0, Interval<size_t>()));
    SmallVector<BufferT *> xBuffers = buffers;
    while (!xBuffers.empty()) {
      auto tripleIt = tripleMap.begin();
      auto offset = tripleIt->first;
      auto range = tripleIt->second;
      tripleMap.erase(tripleIt);
      auto bufferIt =
          std::find_if(xBuffers.begin(), xBuffers.end(), [&](auto *buffer) {
            auto xRange = bufferRange[buffer];
            bool res = xRange.intersects(range);
            for (auto val : tripleMap)
              res = res &&
                    !val.second.intersects(xRange); // only one buffer intersect
            return res;
          });
      if (bufferIt != xBuffers.end()) {
        auto buffer = *bufferIt;
        auto xSize = buffer->size;
        auto xRange = bufferRange.lookup(buffer);
        // TODO(Keren): A buffer's size shouldn't be determined here, have to
        // clean it up
        size_t alignOffset = buffer->setOffsetAligned(offset);
        tripleMap.insert({alignOffset + xSize,
                          Interval{std::max(range.start(), xRange.start()),
                                   std::min(range.end(), xRange.end())}});
        // We could either insert (range.start, xRange.start) or (range.start,
        // xRange.end), both are correct and determine the potential buffer
        // offset, and the graph coloring algorithm will solve the interference,
        // if any
        if (range.start() < xRange.start())
          tripleMap.insert({offset, Interval{range.start(), xRange.end()}});
        if (xRange.end() < range.end())
          tripleMap.insert({offset, Interval{xRange.start(), range.end()}});
        xBuffers.erase(bufferIt);
      }
    }
  }

  /// Builds a graph of all shared memory values. Edges are created between
  /// shared memory values that are overlapping.
  void buildInterferenceGraph(const SmallVector<BufferT *> &buffers,
                              GraphT &interference) {
    // Reset interference graph
    interference.clear();
    for (auto x : buffers) {
      for (auto y : buffers) {
        if (x == y)
          continue;
        auto xStart = x->offset;
        auto yStart = y->offset;
        auto xSize = x->size;
        auto ySize = y->size;
        Interval xSizeRange = {xStart, xStart + xSize};
        Interval ySizeRange = {yStart, yStart + ySize};
        auto xOpRange = bufferRange.lookup(x);
        auto yOpRange = bufferRange.lookup(y);
        if (xOpRange.intersects(yOpRange) &&
            xSizeRange.intersects(ySizeRange)) {
          interference[x].insert(y);
        }
      }
    }
  }

  /// Finalizes shared memory offsets considering interference.
  void allocate(const SmallVector<BufferT *> &buffers,
                const GraphT &interference) {
    // Reset shared memory size
    allocation->setSharedMemorySize(0);
    // First-fit graph coloring
    // Neighbors are nodes that interfere with each other.
    // We color a node by finding the index of the first available
    // non-neighboring node or the first neighboring node without any color.
    // Nodes with the same color do not interfere with each other.
    DenseMap<BufferT *, int> colors;
    for (auto value : buffers) {
      colors[value] = (value == buffers[0]) ? 0 : -1;
    }
    SmallVector<bool> available(buffers.size());
    for (auto x : buffers) {
      std::fill(available.begin(), available.end(), true);
      for (auto y : interference.lookup(x)) {
        int color = colors[y];
        if (color >= 0) {
          available[color] = false;
        }
      }
      auto it = std::find(available.begin(), available.end(), true);
      colors[x] = std::distance(available.begin(), it);
    }
    // Finalize allocation
    // color0: [0, 7), [0, 8), [0, 15) -> [0, 7), [0, 8), [0, 15)
    // color1: [7, 9) -> [0 + 1 * 15, 9 + 1 * 15) -> [15, 24)
    // color2: [8, 12) -> [8 + 2 * 15, 12 + 2 * 15) -> [38, 42)
    // TODO(Keren): We are wasting memory here.
    // Nodes with color2 can actually start with 24.
    for (auto x : buffers) {
      size_t newOffset = 0;
      for (auto y : interference.lookup(x)) {
        newOffset = std::max(newOffset, y->offset + y->size);
      }
      if (colors.lookup(x) != 0)
        x->setOffsetAligned(newOffset);
      allocation->setSharedMemorySize(
          std::max(allocation->getSharedMemorySize(), x->offset + x->size));
    }
  }

private:
  Operation *operation;
  Allocation::FuncAllocMapT *funcAllocMap;
  Allocation *allocation;
  BufferRangeMapT bufferRange;
};

} // namespace triton::intel

template <>
void Allocation::run<triton::intel::AllocationAnalysis>(
    FuncAllocMapT &funcAllocMap) {
  triton::intel::AllocationAnalysis(getOperation(), &funcAllocMap, this);
}

} // namespace mlir