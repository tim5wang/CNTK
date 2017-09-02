//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// TensorView.cpp -- main CPP file that contains all functions exported by the CNTKMath.dll
//
//

// TODO:
//  - dimension inference in nodes
//  - reduction on GPU is highly inefficient; test cases Image/QuickE2E PlusNode::BackpropTo() and ScaleNode::BackpropTo()
//  - accuracy deviation in FullUtterance and SequenceTraining
//  - TimesNode  --needs to specify reduction dimensions
//  - ConvolutionNode   --needs to specify reduction dimensions
//  - some nodes create new "dimensions" such as RowStack. Should that be an actual new tensor dimension?

// This implements the TensorView class, which is a layer around Matrix that reinterprets its content as a generic tensor.
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "stdafx.h"
#include "Basics.h"
#include "TensorView.h"
#include <array>

#ifndef let
#define let const auto
#endif

namespace Microsoft { namespace MSR { namespace CNTK {

using namespace std;

// -------------------------------------------------------------------
// construction
// -------------------------------------------------------------------

// main constructor (all constructors except the default one route through this)
template <class ElemType>
TensorView<ElemType>::TensorView(const MatrixBasePtr& sob, const TensorShape& shape)
    : m_sob(dynamic_pointer_cast<Matrix<ElemType>>(sob)), m_shape(shape)
{
    if (!m_sob)
        LogicError("TensorView: Attempted to create a TensorView<ElemType> on a storage object of a different ElemType.");
#ifdef _DEBUG
    // check bounds of TensorShape against underlying storage object
    // This is useful to detect errors like passing a matrix from the wrong input.
    const auto r = shape.GetLocationRange();
    const auto n = m_sob->GetNumElements();
    if (r.first < 0 || (size_t)r.second > n)
        LogicError("TensorView: Shape bounds [%d,%d) exceed bounds of underlying storage object [0,%d).", (int) r.first, (int) r.second, (int) n);
#endif
}

// -------------------------------------------------------------------
// elementwise operations
// -------------------------------------------------------------------

// look up an op code by name
#define AssignNameToOpTable(oper) insert(make_pair(L#oper, ElementWiseOperator::op##oper));
static struct NameToOpTable : public map<wstring, ElementWiseOperator> { NameToOpTable() { ForAllElementWiseOps(AssignNameToOpTable); } } s_nameToOp;
template <class ElemType>
/*static*/ ElementWiseOperator TensorView<ElemType>::OpFromName(const wstring& opName)
{
    let iter = s_nameToOp.find(opName);
    if (iter != s_nameToOp.end())
        return iter->second;
    else
        InvalidArgument("TensorView::OpFromName: '%S' is not a valid TensorView operation code.", opName.c_str());
}

static bool Matches(size_t d1, size_t d2) // do two dimensions match?
{
    return d1 == d2 || d1 == 1 || d2 == 1; // same or broadcasting
}

template <class ElemType, size_t N>
static void PrepareTensorOperands(array<TensorShape, N> shapes, array<size_t, N>& offsets,
                                  SmallVector<size_t>& regularOpDims,
                                  array<SmallVector<ptrdiff_t>, N>& regularStrides,
                                  SmallVector<size_t>& reducingOpDims,
                                  array<SmallVector<ptrdiff_t>, N>& reducingStrides)
{
    // massage TensorShapes
    // Note that TensorShapes here may be shapes are stored or shapes with stride magic applied.

    // expand ones to make tensors compatible
    // Trailing dimensions broadcast.
    // E.g. A(J) vs. B(J x T) will broadcast A(:) to all T columns.
    // To broadcast an A(T) to all J rows of B, use TensorShape editing to insert a dimension to get A(1,T).
    // We require a minimum rank of 1 (rank 0 is a scalar), as some code may rely on it.
    size_t dims = 1;
    for (size_t i = 0; i < N; i++)
        if (dims < shapes[i].GetRank())
            dims = shapes[i].GetRank();
    for (size_t i = 0; i < N; i++)
        if (shapes[i].GetRank() < dims)
            shapes[i].PadRankInPlace(dims);
    // all shapes[] now have the same rank

    // determine operation shape (max over all dimensions)
    SmallVector<size_t> opDims(shapes[0].GetDims());
    for (size_t k = 0; k < dims; k++)
        for (size_t i = 1; i < N; i++)
            opDims[k] = max(opDims[k], shapes[i][k]);

    // dimension compatibility check
    // Each participant can broadcast. Non-broadcasting dimensions must match the operation dimension.
    for (size_t k = 0; k < dims; k++)
        for (size_t i = 0; i < N; i++)
            if (!Matches(shapes[i][k], opDims[k]))
                InvalidArgument("Binary tensor operation: Dimension %d of input [%d] is incompatible with operation dimensions (%s vs. %s)", (int) k, (int) i, string(shapes[i]).c_str(), string(TensorShape(opDims)).c_str());

    // flatten consecutive dimensions
    // Dimensions must be consecutive in memory, and either non-broadcasting or all-broadcasting, across all dimensions.
    // After this, as, bs, and cs no longer match the TensorShape objects.
    // fprintf(stderr, "Pre-flatten: Op %d: %s op %s -> %s via %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());
    for (size_t k = 1; k < dims; k++)
    {
        for (size_t i = 0; i < N; i++)
        {
            // check if stored without gaps to skip
            if (!shapes[i].CanFlatten(k))
                goto nope;
            // check if they are either all broadcasting or all not broadcasting
            if ((shapes[i][k] != opDims[k] || shapes[i][k - 1] != opDims[k - 1]) && (shapes[i][k] != 1 || shapes[i][k - 1] != 1))
                goto nope;
        }
        // these dimensions can be merged
        for (size_t i = 0; i < N; i++)
            shapes[i].FlattenInPlace(k);                          // TODO: overdoing the immutable thingy much?
        opDims = TensorShape(opDims).FlattenInPlace(k).GetDims(); // (ugh)
    nope:;
    }
    // fprintf(stderr, "Post-flatten: Op %d: %s op %s -> %s via %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());

    // remove singleton dimensions
    SmallVector<bool> toDrop(dims, false);
    bool anyToDrop = false;
    for (size_t k = 0; k < dims; k++)
    {
        for (size_t i = 0; i < N; i++)
            if (shapes[i][k] != 1)
                goto neither;
        toDrop[k] = true; // found an all-singleton dimensions
        anyToDrop = true;
    neither:;
    }
    if (anyToDrop)
    {
        for (size_t i = 0; i < N; i++)
            shapes[i].DropDimsInPlace(toDrop);
        opDims = TensorShape(opDims).DropDimsInPlace(toDrop).GetDims(); // (ugh)
        dims = opDims.size();                                           // #dims has changed
    }
    for (size_t i = 0; i < N; i++)
        assert(dims == shapes[i].size());
    // note: if op is a scalar, then we end up with 0 dimensions here, which is allowed
    // fprintf(stderr, "Post-drop: Op %d: %s op %s -> %s via %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());

    // determine broadcasting; that is, set strides to 0 for 1-dimensions
    // To be more precise, we should only set actually broadcasting dimensions to 0.
    // But since dimensions that are 1 across all args are eliminated, any 1 must be some form of broadcasting.
    for (size_t i = 0; i < N; i++) // TODO: do we need to test output tensor here as well?
        for (size_t k = 0; k < dims; k++)
            if (shapes[i][k] < opDims[k])
            {
                shapes[i].SetBroadcastStrides();
                break;
            }

    // fprintf(stderr, "%s  op  %s  ->  %s  via  %s\n", string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());

    // determine inverse broadcasting dimensions
    // Inverse broadcasting dims are actual for loops in the kernel, whereas broadcasting input dims are handled by the thread index.
    // For regular input dims:
    //  - determine number of steps (product over opDims[.])
    //  - launch that many kernels
    //  - pass in:
    //     - total number of steps
    //     - strides for all inputs (with stride magic), separated by regular and inverse broadcasting dimensions
    //     - opDim (no stride magic allowed) for regular broadcasting dimensions
    //     - reverse broadcasting dimensions
    //     - opcodes for elementwise op and reduction op
    //  - in each kernel:
    //     - map thread index to dimensions (regular broadcasting ones)
    //     - for-loop over inverse broadcasting dimensions
    //        - map dimensions (including inverse broadcasting) for every input
    //        - perform op on the input values
    //        - accumulate
    //     - map dimensions (regular) for output
    //     - save result

    // separate out the inverse-broadcasting dimensions
    // Any singleton dimension in the result tensor is inverse-broadcasting, because there must be at least one non-1 dimension
    // in one of the inputs, otherwise the entire dimension would have been optimized away above.
    SmallVector<bool> isReducingDim(dims); // true for each inverse-broadcasting dimension
    bool isAnyReducingDim = false;
    for (size_t k = 0; k < dims; k++)
    {
        bool isRed = shapes.back()[k] == 1;
        isReducingDim[k] = isRed;
        isAnyReducingDim |= isRed;
    }

    // form the regular (non-inverse-broadcasting) dims
    if (isAnyReducingDim)
    {
        for (size_t i = 0; i < N; i++)
            regularStrides[i] = shapes[i].DropDims(isReducingDim).GetStrides();
        regularOpDims = TensorShape(opDims).DropDims(isReducingDim).GetDims(); // (ugh)

        // form the inverse-broadcasting dims
        SmallVector<bool> isRegularDim(dims); // true for each inverse-broadcasting dimension
        for (size_t k = 0; k < dims; k++)
            isRegularDim[k] = !isReducingDim[k]; // (no way to do this more nicely?)
        for (size_t i = 0; i < N; i++)
            reducingStrides[i] = shapes[i].DropDims(isRegularDim).GetStrides();
        reducingOpDims = TensorShape(opDims).DropDims(isRegularDim).GetDims(); // (ugh)
    }
    else // case if no reduction: things are simpler
    {
        for (size_t i = 0; i < N; i++)
            regularStrides[i] = shapes[i].GetStrides();
        regularOpDims = opDims;

        for (size_t i = 0; i < N; i++)
            reducingStrides[i].clear();
        reducingOpDims.clear();
    }

    for (size_t i = 0; i < N; i++)
        offsets[i] = shapes[i].GetOffset();
}

// enforce that in case of broadcasting, the output must not be an input
template <class ElemType>
static bool CheckDifferentObject(const TensorView<ElemType>& a, const TensorView<ElemType>& b)
{
    if (&a == &b)
        LogicError("Do{U,Bi,Ter}naryOpOf: When inverse broadcasting, output must not be an input.");
    return true;
}

template <class ElemType>
void TensorView<ElemType>::DoNullaryOpOf(ElemType beta, ElemType alpha, ElementWiseOperator op, ElementWiseOperator reductionOp)
{
    // A nullary op cannot reduce, but we keep it regular anyways.
    // prepare all tensor descriptor information as needed for execution
    array<size_t, 1> offsets;
    array<SmallVector<ptrdiff_t>, 1> regularStrides, reducingStrides;
    SmallVector<size_t> regularOpDims, reducingOpDims;
    PrepareTensorOperands<ElemType, 1>(array<TensorShape, 1>{GetShape()}, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);

    // now perform the operation
    GetSOB().TensorOp(beta, alpha, op, reductionOp, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
}

template <class ElemType>
void TensorView<ElemType>::DoUnaryOpOf(ElemType beta, const TensorView& a, ElemType alpha, ElementWiseOperator op, ElementWiseOperator reductionOp)
{
    // static int cc = 0; if (cc++ == 0)
    //    fprintf(stderr, "Tensor Op: Op %d: %s -> %s\n", (int)op, string(a.GetShape()).c_str(), string(GetShape()).c_str());

    // prepare all tensor descriptor information as needed for execution
    array<size_t, 2> offsets;
    array<SmallVector<ptrdiff_t>, 2> regularStrides, reducingStrides;
    SmallVector<size_t> regularOpDims, reducingOpDims;
    PrepareTensorOperands<ElemType, 2>(array<TensorShape, 2>{a.GetShape(), GetShape()}, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);

    // output cannot be input when reducing
    if (reducingOpDims.size() > 0)
        CheckDifferentObject(a, *this);

    // now perform the operation
    GetSOB().TensorOp(beta, a.GetSOB(), alpha, op, reductionOp, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
}

template <class ElemType>
void TensorView<ElemType>::DoBinaryOpOf(ElemType beta, const TensorView& a, const TensorView& b, ElemType alpha, ElementWiseOperator op, ElementWiseOperator reductionOp)
{
    // static int cc = 0; if (cc++ == 0)
    //    fprintf(stderr, "Tensor Op: Op %d: %s op %s -> %s\n", (int)op, string(a.GetShape()).c_str(), string(b.GetShape()).c_str(), string(GetShape()).c_str());

    // TODO: route matrix product (and convolution later) through this API as well.

    array<size_t, 3> offsets;                                         // [argIndex] (where result goes into last arg)
    array<SmallVector<ptrdiff_t>, 3> regularStrides, reducingStrides; // [argIndex][axisIndex] (axisIndex after flattening; same dims as regular/reducingOpDims)
    SmallVector<size_t> regularOpDims, reducingOpDims;                // [axisIndex]
    PrepareTensorOperands<ElemType, 3>(array<TensorShape, 3>{a.GetShape(), b.GetShape(), GetShape()}, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);

    // output cannot be input when reducing
    if (reducingOpDims.size() > 0)
        CheckDifferentObject(a, *this) && CheckDifferentObject(b, *this);

    // special support for sparse data: ReduceSum(ElementWiseProduct(x,y)) (same as batched Times(x,y)) and gradient.
    // This is used for batched cross-entropy computation.
    if (op == ElementWiseOperator::opElementwiseProduct && reductionOp == ElementWiseOperator::opSum)
    {
        // Note: Because CNTK API does not allow ElementTimes with ReduceSum in one op, user writes Times() instead, and Dynamite will map it to this instead.
        let IsDotProduct = [&]() -> bool // helper to decide
        {
            // must be reducing consecutive input values into one result value
            // regularXX represents the map dimension; e.g. [13 x 3  x  42 x 5] * [13 x 3  x  42 x 5] --> [1 x 1  x  42 x 5]
            // gets a reduction over 13*3 consecutive values, with result being consecutive in memory arranged in a 42 x 5 grid
            if (reducingOpDims.size() != 1 || reducingStrides[0][0] != 1 || reducingStrides[1][0] != 1 || reducingStrides[2][0] != 0)
                return false;
            // inputs must be consecutive in memory also for the non-reduced axes (which gets flattened into one if condition is true)
            if (regularOpDims.size() != 0) // (only if there are non-reduced axes)
            {
                let reducedElements = (int)reducingOpDims[0];
                if (regularOpDims.size() != 1 || regularStrides[0][0] != reducedElements || regularStrides[1][0] != reducedElements || regularStrides[2][0] != 1)
                    return false;
            }
            return true; // that's it
        };
        // dot product
        if (IsDotProduct())
        {
            let remainingElements =   GetShape().GetNumElements();                     // keeping this many elements
            let reducedElements   = a.GetShape().GetNumElements() / remainingElements; // summing up this many elements per result
            let inShape  = TensorShape(reducedElements, remainingElements);
            let outShape = TensorShape(1,               remainingElements);
            let  A = a.Reshaped(inShape).AsMatrix();
            let  B = b.Reshaped(inShape).AsMatrix();
            auto C =   Reshaped(outShape).AsMatrix();
            return Matrix<ElemType>::InnerProduct(*A, *B, *C, true/*isColWise*/);
        }
        let IsDotProductGradient = [&]() -> bool // helper to decide
        {
            // there must be no reduction
            if (reducingOpDims.size() != 0)
                return false;
            // at least one input must be broadcasting in the first flattened dimension, and must have at most one additional flattened dimension without broadcasting
            if (regularOpDims.size() > 2) // input has too many non-flattened axes, not representable as a matrix
                return false;
            if (regularOpDims.size() > 0) // check the broadcasting dimension (may be missing to cater for degenerate case of all inputs being scalars)
            {
                if (regularStrides[0][0] != 0 && regularStrides[1][0] != 0) // one of them must broadcast
                    return false;
                if (regularStrides[0][0] > 1 || regularStrides[1][0] > 1) // broadcasting dimension must be consecutive
                    return false;
            }
            if (regularOpDims.size() > 1) // check the "batch" dimension
            {
                let aHeight = regularStrides[0][0] == 0 ? 1 : (int)regularOpDims[0];
                let bHeight = regularStrides[1][0] == 0 ? 1 : (int)regularOpDims[0];
                if (regularStrides[0][1] != aHeight || regularStrides[1][1] != bHeight) // batch dimension must be consecutive in memory
                    return false;
            }
            return true; // that's it
        };
        // gradient of dot product: scalar * vector -> vector
        if (IsDotProductGradient())
        {
            let aIsWeight = regularOpDims.size() == 0 || regularStrides[0][0] == 0; // which of the two inputs is the weight? We allow both ways.
            let&   data = !aIsWeight ? a : b;
            let& weight =  aIsWeight ? a : b;
            let width  = weight.GetShape().GetNumElements();         // number of scalar weights =  "batch dim"
            let height =   data.GetShape().GetNumElements() / width; // broadcasting into this many elements per result
            let   dataShape = TensorShape(height, width);
            let weightShape = TensorShape(1,      width);
            let  A =   data.Reshaped(  dataShape).AsMatrix();
            let  B = weight.Reshaped(weightShape).AsMatrix(); // the weight is the second argument to ColumnwiseScaleAndWeightedAdd()
            auto C =        Reshaped(  dataShape).AsMatrix();
            return Matrix<ElemType>::ColumnwiseScaleAndWeightedAdd((ElemType)1.0, *A, *B, beta, *C);
        }
    }

    // regular case
    GetSOB().TensorOp(beta, a.GetSOB(), b.GetSOB(), alpha, op, reductionOp, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
}

template <class ElemType>
void TensorView<ElemType>::DoTernaryOpOf(ElemType beta, const TensorView& a, const TensorView& b, const TensorView& c, ElemType alpha, ElementWiseOperator op, ElementWiseOperator reductionOp)
{
    // static int cc = 0; if (cc++ == 0)
    //    fprintf(stderr, "Tensor Op: Op %d: %s, %s, %s -> %s\n", (int)op, string(a.GetShape()).c_str(), string(b.GetShape()).c_str(), string(c.GetShape()).c_str(), string(GetShape()).c_str());

    array<size_t, 4> offsets;
    array<SmallVector<ptrdiff_t>, 4> regularStrides, reducingStrides;
    SmallVector<size_t> regularOpDims, reducingOpDims;
    PrepareTensorOperands<ElemType, 4>(array<TensorShape, 4>{a.GetShape(), b.GetShape(), c.GetShape(), GetShape()}, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);

    // output cannot be input when reducing
    if (reducingOpDims.size() > 0)
        CheckDifferentObject(a, *this) && CheckDifferentObject(b, *this) && CheckDifferentObject(c, *this);

    GetSOB().TensorOp(beta, a.GetSOB(), b.GetSOB(), c.GetSOB(), alpha, op, reductionOp, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
}

template <class ElemType>
void TensorView<ElemType>::DoQuaternaryOpOf(ElemType beta, const TensorView& a, const TensorView& b, const TensorView& c, const TensorView& d, ElemType alpha, ElementWiseOperator op, ElementWiseOperator reductionOp)
{
    array<size_t, 5> offsets;
    array<SmallVector<ptrdiff_t>, 5> regularStrides, reducingStrides;
    SmallVector<size_t> regularOpDims, reducingOpDims;
    PrepareTensorOperands<ElemType, 5>(array<TensorShape, 5>{a.GetShape(), b.GetShape(), c.GetShape(), d.GetShape(), GetShape()}, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);

    // output cannot be input when reducing
    if (reducingOpDims.size() > 0)
        CheckDifferentObject(a, *this) && CheckDifferentObject(b, *this) && CheckDifferentObject(c, *this) && CheckDifferentObject(d, *this);

    GetSOB().TensorOp(beta, a.GetSOB(), b.GetSOB(), c.GetSOB(), d.GetSOB(), alpha, op, reductionOp, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
}

template <class ElemType>
void TensorView<ElemType>::DoArgReductionOpOf(const TensorView& a, ElementWiseOperator reductionOp)
{
    // prepare all tensor descriptor information as needed for execution
    array<size_t, 2> offsets;
    array<SmallVector<ptrdiff_t>, 2> regularStrides, reducingStrides;
    SmallVector<size_t> regularOpDims, reducingOpDims;
    PrepareTensorOperands<ElemType, 2>(array<TensorShape, 2>{a.GetShape(), GetShape()}, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);

    // output cannot be input when reducing
    if (reducingOpDims.size() > 0)
        CheckDifferentObject(a, *this);

    // now perform the operation
    GetSOB().TensorArgOp(a.GetSOB(), reductionOp, offsets, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
}

// -------------------------------------------------------------------
// matrix product -- GEMM for flattened tensors
// -------------------------------------------------------------------

// print the dimensions of a matrix-product operation, for pretty error reporting
static string MatrixProductFormat(const TensorShape& shapeA, bool transA, const TensorShape& shapeB, bool transB, const TensorShape& shapeC, bool transC)
{
    string result = "[" + string(shapeA) + "]"; if (transA) result.append("'");
    result += " * ";
    result +=       "[" + string(shapeB) + "]"; if (transB) result.append("'");
    result += " -> ";
    result +=       "[" + string(shapeC) + "]"; if (transC) result.append("'");
    return result;
}

// flatten a tensor into a 2D tensor, where splitPoint is the first index to go into the second dimension
// The tensor must be flattenable this way, i.e. each of the two index ranges must be dense.
static void FlattenToMatrix(TensorShape& shape, bool trans, size_t splitPoint)
{
    if (trans)
        splitPoint = shape.GetRank() - splitPoint;

    shape.FlattenTo2DInPlace(splitPoint, "DoMatrixProductOf");
}

// convert tensor into a Matrix object
// BUGBUG: Rethink whether for rank < 2, padding ones at the end is correct when the matrix is meant to be transposed.
template <class ElemType>
shared_ptr<Matrix<ElemType>> TensorView<ElemType>::AsMatrix() const
{
    if (m_shape.GetRank() > 2)
        InvalidArgument("AsMatrix: The [%s] tensor has too many axes to be interpreted as a matrix (max 2).", string(m_shape).c_str());

    let m_shape_0 = m_shape.GetRank() > 0 ? m_shape[0] : 1;
    let m_shape_1 = m_shape.GetRank() > 1 ? m_shape[1] : 1;

    if (m_shape.GetRank() > 0 && m_shape.GetStrides()[0] != 1 && m_shape_0 != 1)
        InvalidArgument("AsMatrix: Flattened [%s] matrix is not dense (it has a stride).", string(m_shape).c_str());

    let sobRows = m_sob->GetNumRows();
    let sobCols = m_sob->GetNumCols();
    let viewElements = m_shape.GetNumElements();

    // now reinterpret this slice according to the new tensor shape
    // Example:
    //  - each sob column contains a set of vectors stored as a 2D tensor [I x J], and [S x T] samples
    //  - we want to apply a [K x I] matrix to all vectors in each set
    //  - so we reinterpret the [(I * J) x (S * T)] storage object as a [I x (J * S * T)] matrix
    //    and apply the matrix product to this (by calling GEMM)
    //  - which in turn yields a [K x (J * S x*T)] matrix
    //    which gets reinterpreted back as a [K x J x S x T] tensor
    // In the special case of sparse matrices, this split cannot be done. E.g. in the above example, we could only multiply with a [K x I x J] tensor.
    let needsSlicing = viewElements != sobRows * sobCols;
    let needsReshaping = m_shape_0 != sobRows || m_shape_1 != sobCols;

    // Note: If an output matrix is a view and needs to move to a different device, we will fail later, since the current structure cannot support that.
    // As a consequence, some configurations will simply not work currently.
    // We minimize the chance of this by using the original storage object whenever possible.

    // if the SOB is already correct, then return it unmodified. This allows full support for moving devices.
    if (!needsSlicing && !needsReshaping)
        return m_sob;

    if (m_sob->GetMatrixType() != MatrixType::DENSE) // sparse
    {
        // Sparse matrices can be column-sliced; that's it.
        if (needsReshaping) // not allowed for sparse matrices
            RuntimeError("AsMatrix: Sparse tensors are not supported unless they are 1D or 2D matrices.");
        assert(needsSlicing);
        let firstColumn = m_shape.GetOffset() / sobRows;
        let numColumns  = viewElements        / sobRows;
        if (firstColumn * sobRows != m_shape.GetOffset() || numColumns * sobRows != viewElements)
            InvalidArgument("AsMatrix: Flattened [%s] matrix has an offset or width that is not a multiple of the storage object's row dimension.", string(m_shape).c_str());
        return make_shared<Matrix<ElemType>>(move(m_sob->ColumnSlice(firstColumn, numColumns)));
    }
    else // dense
    {
        // Dense matrices can be arbitrarily reshaped and sliced. We fist slice from a row vector, and then reshape it.
        auto slice = m_sob->ColumnSlice(m_shape.GetOffset(), viewElements, /*pretendSourceHasNumCols=*/m_sob->GetNumElements());
        slice.Reshape(m_shape_0, m_shape_1);
        return make_shared<Matrix<ElemType>>(move(slice));
    }
}

template <class ElemType>
void TensorView<ElemType>::DoMatrixProductOf(ElemType beta, bool transC, const TensorView& a, bool transA, const TensorView& b, bool transB, ElemType alpha, shared_ptr<QuantizedMultiplier<ElemType>> pQuantizedMultiplier)
{
    // determine integration dimension offset
    auto shapeA = a.m_shape;
    auto shapeB = b.m_shape;
    auto shapeC =   m_shape;
    if (shapeA.size() == 1) // if just a vector then make it a row vector; this is like Numpy
        transA = shapeB.size() > 0; // (original transA value is ignored; it's just a vector)
    if (shapeA.GetRank() + shapeB.GetRank() < shapeC.GetRank())
        InvalidArgument("DoMatrixProductOf: Ranks %s don't match, output must have a non-reduced output dimension.", MatrixProductFormat(shapeA, transA, shapeB, transB, shapeC, transC).c_str());
    let removedDims = shapeA.GetRank() + shapeB.GetRank() - shapeC.GetRank();
    let numReducedDims = removedDims / 2;
    if (numReducedDims * 2 != removedDims)
        InvalidArgument("DoMatrixProductOf: Ranks %s mismatch.", MatrixProductFormat(shapeA, transA, shapeB, transB, shapeC, transC).c_str());
    let firstReducedDim = shapeA.GetRank() - numReducedDims;
    // flatten. This updates shapeA etc.
    FlattenToMatrix(shapeA, transA, firstReducedDim);
    FlattenToMatrix(shapeB, transB, numReducedDims);
    FlattenToMatrix(shapeC, transC, firstReducedDim);
    // check dimensions
    // shapeX[transX] and shapeX[1-transX] are row and column dim, respectively, or swapped if transposed
    if (shapeA[transA]   != shapeC[transC]   || // output dim
        shapeB[1-transB] != shapeC[1-transC] || // input dim
        shapeA[1-transA] != shapeB[transB])     // reduction dim
    {
        InvalidArgument("DoMatrixProductOf: Flattened tensor dimensions %s mismatch.", MatrixProductFormat(shapeA, transA, shapeB, transB, shapeC, transC).c_str());
    }
    // create Matrix objects out of this
    // BUGBUG: AsMatrix() may need to take a transposed flag, as to know where to pad?
    let  A = a.Reviewed(shapeA).AsMatrix();
    let  B = b.Reviewed(shapeB).AsMatrix();
    auto C =   Reviewed(shapeC).AsMatrix();
    // and go
    if (!transC)
        Matrix<ElemType>::MultiplyAndWeightedAdd(alpha, *A, transA, *B, transB, beta, *C, pQuantizedMultiplier);
    else // C' = A * B  <==>  C = (A * B)' = B' * A'
        Matrix<ElemType>::MultiplyAndWeightedAdd(alpha, *B, !transB, *A, !transA, beta, *C, pQuantizedMultiplier);
}

// -------------------------------------------------------------------
// gather batch -- splice multiple TensorViews into a batch
// scatter batch -- redistribute a gathered batch into multiple TensorViews
// -------------------------------------------------------------------

#if 0
// helper to report mismatching dimensions for DoGather/ScatterBatchOf()
// Item = the individual item (Gather: input; Scatter: output)
// Batch = the batched item (Gather: this (=output); Scatter: this (=input))
static void GatherScatterVerifyDimensions(const TensorShape& itemShape, const TensorShape& batchedShape,
                                          const char* funcName, size_t i) // <- for error messages
{
    // item must not have more axes than batch
    if (itemShape.GetRank() > batchedShape.GetRank())
        InvalidArgument("%s: Item %d's shape %s has more axes than the batched shape %s.",
                        funcName, (int)i, string(itemShape).c_str(), string(batchedShape).c_str());
    let n = min(itemShape.GetRank(), batchedShape.GetRank() - 1);
    for (size_t k = 0; k < n; k++)
        if (itemShape[k] != batchedShape[k])
            InvalidArgument("%s: Item %d's shape %s mismatches the batched shape %s.",
                            funcName, (int)i, string(itemShape).c_str(), string(batchedShape).c_str());
    for (size_t k = n; k < batchedShape.GetRank() - 1; k++)
        if (1 != batchedShape[k])
            InvalidArgument("%s: Item %d's shape %s missing non-1 dimensions of batched shape %s.",
                            funcName, (int)i, string(itemShape).c_str(), string(batchedShape).c_str());
}
#endif

// helper to determine whether the SOB can be passed as is (-> true), or needs to be converted into a view (-> false)
template <class ElemType>
static bool GatherScatterCanPassSOB(const TensorView<ElemType>& itemView)
{
    let& shape = itemView.GetShape();
    let& sob = itemView.GetSOB();
    shape.VerifyIsDense(); // we don't support non-dense tensors here
    return shape.GetNumElements() == sob.GetNumElements() && shape.GetOffset() == 0;
    // Note: Comparing the number of elements is sufficient to know whether there are gaps.
    // It is not sufficient to know whether axes have been transposed, but we also verified it is dense.
    // We do not test for sparse in this condition because sparse views are always created with correct
    // matrix dimensions. Any mismatch will be caught elsewhere.
}

template <class ElemType>
static Matrix<ElemType>& GatherScatterGetSOBView(const TensorView<ElemType>& itemView, Matrix<ElemType>& out)
{
    // (it has already been verified in GatherScatterCanPassSOB() that the matrix is contiguous in memory)
    let& shape = itemView.GetShape();
    let& sob = itemView.GetSOB();
    // create a single-row view into the buffer
    if (sob.GetMatrixType() != MatrixType::DENSE) // sparse
    {
        let sobRows = sob.GetNumRows();
        let viewElements = shape.GetNumElements();
        let firstColumn = shape.GetOffset() / sobRows;
        let numColumns  = viewElements      / sobRows;
        if (firstColumn * sobRows != shape.GetOffset() || numColumns * sobRows != viewElements)
            InvalidArgument("GatherScatterGetSOBView: Sparse [%s] tensor has an offset or width that is not a multiple of the storage object's row dimension.", string(shape).c_str());
        out = move(sob.ColumnSlice(firstColumn, numColumns));
    }
    else // dense
    {
        out = move(sob.ColumnSlice(shape.GetOffset(), shape.GetNumElements(), /*pretendSourceHasNumCols=*/sob.GetNumElements()));
    }
    return out;
}

template<typename TArrayRef> // (const) TensorView*
static bool CanGatherScatterBatch(const TensorShape& m_shape, const TArrayRef& inputs, size_t axis)
{
    let arity = inputs.size();
    // check whether this can be a Gather
    if (m_shape.GetRank() == 0)
        InvalidArgument("DoGatherBatchOf: Output cannot be a scalar.");
    let outRank = m_shape.GetRank();
    let& shape0 = inputs[0]->GetShape();
    bool canGather = (axis == outRank - 1) && shape0.IsDense();
    // all shapes must be identical to the outputShape with splice axis divided by #arguments
    // check first shape
    for (size_t k = 0; canGather && k < outRank; k++)
    {
        auto dim = k < shape0.GetRank() ? shape0[k] : 1;
        if (k == axis)
            dim *= arity;
        canGather &= (dim == m_shape[k]);
    }
    // first shape is the correct fraction: check all shapes against it (hah--no malloc!)
    for (size_t j = 1; canGather && j < arity; j++)
    {
        let& shapej = inputs[j]->GetShape();
        canGather &= shapej.GetDims() == shape0.GetDims() && shapej.IsDense();
    }
    return canGather;
}

template <class ElemType>
void TensorView<ElemType>::DoGatherBatchOf(const IArrayRef<const TensorView*>& inputs, size_t axis)
{
    // Batches inputs along an axis.
    // A special optimization is applied when each input is dense and its shape does, 1-padded, match
    // the output shape except for the last output dimension, which is the sum of the (1-padded) input dimensions.
    if (CanGatherScatterBatch(m_shape, inputs, axis))
    {
        // optimized case
        let numRows = m_shape.GetNumElements() / m_shape.GetDims().back();
        Matrix<ElemType> sliceBuf(CPUDEVICE/*dummy*/); // buffer to hold the slice so that we can return it by reference
        GetSOBViewPtr()->GatherBatch(numRows, inputs.size(), [&](size_t i) -> const Matrix<ElemType>&
        {
            let& input = *inputs[i];
            // TODO: Remove this check. We already verified the dimensions outside.
            //GatherScatterVerifyDimensions(input.m_shape, m_shape, "DoGatherBatchOf", i);
            if (GatherScatterCanPassSOB(input))
                return input.GetSOB();
            else
                return GatherScatterGetSOBView(input, sliceBuf);
        });
    }
    else
    {
        // copy all items one by one
        // This is not efficient for many objects (e.g. a batch gather), but fine for 2 or 3.
        size_t sliceStart = 0;
        let arity = inputs.size();
        for (size_t i = 0; i < arity; i++)
        {
            let& input = *inputs[i];
            let& shape = input.m_shape;
            let sliceHeight = axis < shape.GetRank() ? shape[axis] : 1;
            // slice in output
            TensorShape outSlice = m_shape;
            outSlice.NarrowTo(axis, sliceStart, sliceStart + sliceHeight);
            Reviewed(outSlice).AssignCopyOf(input);
            sliceStart += sliceHeight;
        }
    }
}

// WARNING: The function will not detect if outputs overlap. The caller must detect this and pass beta=1, to avoid one erasing the other.
template <class ElemType>
void TensorView<ElemType>::DoScatterBatchOf(ElemType beta, const IArrayRef<TensorView*>& outputs, size_t axis) const
{
    // Redistributes a batched object to outputs along the last axis of 'this'.
    // Each output shape must, 1-padded, match the input ('this') shape except for the last input
    // dimension, which is the sum of the (1-padded) output dimensions.
    if (CanGatherScatterBatch(m_shape, outputs, axis))
    {
        if (m_shape.GetRank() == 0)
            InvalidArgument("DoScatterBatchOf: Input cannot be a scalar.");
        let numRows = m_shape.GetNumElements() / m_shape.GetDims().back();
        Matrix<ElemType> sliceBuf(CPUDEVICE/*dummy*/);
        GetSOBViewPtr()->ScatterBatch(beta, numRows, outputs.size(), [&](size_t i) -> Matrix<ElemType>&
        {
            auto& output = *outputs[i];
            // TODO: remove this check code, already verified outside
            //GatherScatterVerifyDimensions(output.m_shape, m_shape, "DoScatterBatchOf", i);
            if (GatherScatterCanPassSOB(output))
                return output.GetSOB();
            else
                return GatherScatterGetSOBView(output, sliceBuf);
        });
    }
    else
    {
        // copy all items one by one
        // This is not efficient for many objects (e.g. a batch gather), but fine for 2 or 3.
        size_t sliceStart = 0;
        let arity = outputs.size();
        for (size_t i = 0; i < arity; i++)
        {
            auto& output = *outputs[i];
            let& shape = output.m_shape;
            let sliceHeight = axis < shape.GetRank() ? shape[axis] : 1;
            // slice in input
            TensorShape inSlice = m_shape;
            inSlice.NarrowTo(axis, sliceStart, sliceStart + sliceHeight);
            output.DoCopyOf(beta, Reviewed(inSlice), /*alpha*/1.0);
            sliceStart += sliceHeight;
        }
    }
}

// -------------------------------------------------------------------
// GetSOBViewPtr() -- get a view on the SOB if possible (will fail if TensorView is not contiguous in memory)
// -------------------------------------------------------------------

template <class ElemType>
shared_ptr<Matrix<ElemType>> TensorView<ElemType>::GetSOBViewPtr() const
{
    // return the original if no need for slicing and dicing
    if (GatherScatterCanPassSOB(*this))
        return m_sob;
    else
    {
        m_shape.VerifyIsDense();
        Matrix<ElemType> sliceBuf(CPUDEVICE/*dummy*/);
        GatherScatterGetSOBView(*this, sliceBuf); 
        return make_shared<Matrix<ElemType>>(move(sliceBuf));
    }
}

// -------------------------------------------------------------------
// AsString() -- format a tensor for logging/printing
// -------------------------------------------------------------------

// prints object of subRank from offset
// The print format is similar to numpy, except if columnMajor is specified.
// In that case, the matrix level is printed in Matlab format.
// 'index' is along 'axis'; subRank is recursion depth (rank of current object).
// SubRank and axis are the same except for matrix level (subRank=2); then the axes are transposed if columnMajor.
template <class ElemType, class DimsVecType/*vector or SmallVector*/, class StridesVecType/*vector or SmallVector*/>
static __declspec(noinline) size_t TensorDataAsString(string& res,
                                                      const ElemType* data, const DimsVecType& dims, const StridesVecType& strides,
                                                      size_t subRank, size_t axis, size_t index, size_t maxItems = 6, bool columnMajor = true)
{
    let rank = dims.size();
    // print preceding separator
    if (index > 0)
    {
        if (subRank == 1 && columnMajor)
            res.append(1, ';');
        else
            res.append(1, ',');
        for (size_t n = 0; n < subRank; n++)
            res.append(1, '\n');
        res.append(subRank == 0 ? 2 : (int)(rank - subRank), ' ');
    }
    // print object
    if (index > 0 && dims[axis] > maxItems) // dims[axis] is guaranteed to be valid if index > 0
    {
        if (index == (maxItems + 1) / 2)
        {
            if (columnMajor && subRank == 1)
                res.append(1, ' ');
            res.append(3, '.');
            return dims[axis] - maxItems / 2;
        }
    }
    if (subRank == 0) // scalar: print the item
        res.append(to_string(*data));
    else
    {
        let axis1 = (rank >= 2 && subRank <= 2 && columnMajor) ? 2 - subRank : subRank - 1; // column major for matrix level
        if (!columnMajor || rank < 2 || subRank != 1)
            if (rank > 0)
                res.append(1, '[');
        if (subRank == 1)
            res.append(1, ' ');
        for (size_t index1 = 0; index1 < dims[axis1]; )
            index1 = TensorDataAsString(res, data + index1 * strides[axis1], dims, strides, subRank - 1, axis1, index1, maxItems, columnMajor);
        if (!columnMajor || rank < 2 || subRank != 1)
        {
            if (subRank == 1 || (columnMajor && subRank == 2))
                res.append(1, ' ');
            if (rank > 0)
                res.append(1, ']');
        }
    }
    return index + 1;
}

template <class ElemType>
string TensorView<ElemType>::AsString(size_t maxItems /*= 6*/, bool columnMajor /*= true*/) const
{
    let& sobViewPtr = GetSOBViewPtr();
    unique_ptr<ElemType[]> data(sobViewPtr->CopyToArray());
    let dims    = m_shape.GetDims();
    let strides = m_shape.GetStrides();
    let rank = m_shape.GetRank();
    string res;
    res.reserve(sobViewPtr->GetNumElements() * 5);
    TensorDataAsString(res, data.get(), dims, strides, rank, rank, 0, maxItems, columnMajor);
    return res;
}

template class TensorView<float>;
template class TensorView<double>;

}}}