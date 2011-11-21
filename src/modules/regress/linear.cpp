/* ----------------------------------------------------------------------- *//**
 *
 * @file linear.cpp
 *
 * @brief Linear-regression functions
 *
 *//* ----------------------------------------------------------------------- */

#include MADLIB_DBCONNECTOR_HEADER

// #include <modules/prob/prob.hpp>

namespace madlib {

using utils::Reference;
using utils::MutableReference;

namespace modules {

namespace regress {

#include "linear.hpp"


// Import names from other MADlib modules
//using prob::studentT_cdf;

/**
 * @brief Define types depending on whether transition state is mutable or
 *     immutable
 *
 * HandleTraits are arguably overkill, but they demonstrate how strict type
 * safety and const-correctness can be implemented. Alternatively, we could have
 * used a single <tt>const_cast</tt>.
 */
template <class Handle, class LinAlgTypes>
struct HandleTraits;

template <class LinAlgTypes>
struct HandleTraits<AbstractionLayer::ArrayHandle<double>, LinAlgTypes> {
    typedef typename LinAlgTypes::ColumnVector ColumnVector;
    typedef typename LinAlgTypes::Matrix Matrix;

    typedef AbstractionLayer::TransparentHandle<double> TransparentHandle;
    typedef typename LinAlgTypes::template HandleMap<const ColumnVector>
        ColumnVectorArrayHandleMap;
    typedef Reference<double, uint64_t> ReferenceToUInt64;
    typedef Reference<double, uint16_t> ReferenceToUInt16;
    typedef Reference<double> ReferenceToDouble;
    typedef typename LinAlgTypes::template HandleMap<
        const ColumnVector, TransparentHandle> ColumnVectorTransparentHandleMap;
    typedef typename LinAlgTypes::template HandleMap<
        const Matrix, TransparentHandle> MatrixTransparentHandleMap;
};

template <class LinAlgTypes>
struct HandleTraits<AbstractionLayer::MutableArrayHandle<double>, LinAlgTypes> {
    typedef typename LinAlgTypes::ColumnVector ColumnVector;
    typedef typename LinAlgTypes::Matrix Matrix;

    typedef AbstractionLayer::MutableTransparentHandle<double>
        TransparentHandle;
    typedef typename LinAlgTypes::template HandleMap<ColumnVector>
        ColumnVectorArrayHandleMap;
    typedef MutableReference<double, uint64_t> ReferenceToUInt64;
    typedef MutableReference<double, uint16_t> ReferenceToUInt16;
    typedef MutableReference<double> ReferenceToDouble;
    typedef typename LinAlgTypes::template HandleMap<
        ColumnVector, TransparentHandle> ColumnVectorTransparentHandleMap;
    typedef typename LinAlgTypes::template HandleMap<
        Matrix, TransparentHandle> MatrixTransparentHandleMap;
};

/**
 * @brief Transition state for linear-regression functions
 *
 * TransitionState encapsulates the transition state during the
 * linear-regression aggregate functions. To the database, the state is exposed
 * as a single DOUBLE PRECISION array, to the C++ code it is a proper object
 * containing scalars, a vector, and a matrix.
 *
 * Note: We assume that the DOUBLE PRECISION array is initialized by the
 * database with length at least 5, and all elemenets are 0.
 */
template <class Handle, class LinAlgTypes = DefaultLinAlgTypes>
class LinRegrTransitionState : public AbstractionLayer {
    // By §14.5.3/9: "Friend declarations shall not declare partial
    // specializations."
    template <class OtherHandle, class OtherLinAlgTypes>
    friend class LinRegrTransitionState;

public:
    /**
     * @brief Bind to storage array
     *
     * @internal Array layout ():
     * - 0: numRows (number of rows seen so far)
     * - 1: widthOfX (number of coefficients)
     * - 2: y_sum (sum of independent variables seen so far)
     * - 3: y_square_sum (sum of squares of independent variables seen so far)
     * - 4: X_transp_Y (X^T y, for that parts of X and y seen so far)
     * - 4 + widthOfX + widthOfX % 2: (X^T X, as seen so far)
     *
     * Note that we want 16-byte alignment for all vectors and matrices. We
     * therefore ensure that X_transp_Y and X_transp_X begin at even positions.
     *
     * @internal Member initalization occurs in the order of declaration in the
     *      class (see ISO/IEC 14882:2003, Section 12.6.2). The order in the
     *      init list is irrelevant. It is important that mStorage gets
     *      initialized before the other members!
     */
    LinRegrTransitionState(const AnyType &inArray)
      : mStorage(inArray.getAs<Handle>()) {
        
        rebind(mStorage[1]);
    }
    
    /**
     * We define this function so that we can use TransitionState in the argument
     * list and as a return type.
     */
    inline operator AnyType() const {
        return mStorage;
    }
    
    /**
     * @brief Initialize the transition state. Only called for first row.
     * 
     * @param inAllocator Allocator for the memory transition state. Must fill
     *     the memory block with zeros.
     * @param inWidthOfX Number of independent variables. The first row of data
     *     determines the size of the transition state. This size is a quadratic
     *     function of inWidthOfX.
     */
    inline void initialize(const Allocator &inAllocator, const uint16_t inWidthOfX) {
        mStorage = inAllocator.allocateArray<double>(arraySize(inWidthOfX));
        rebind(inWidthOfX);
        widthOfX = inWidthOfX;
    }
    
    /**
     * @brief Merge with another TransitionState object
     */
    template <class OtherHandle>
    LinRegrTransitionState &operator+=(
        const LinRegrTransitionState<OtherHandle, LinAlgTypes> &inOtherState) {
        
        if (mStorage.size() != inOtherState.mStorage.size())
            throw std::logic_error("Internal error: Incompatible transition states");
            
        for (uint32_t i = 0; i < mStorage.size(); i++)
            mStorage[i] += inOtherState.mStorage[i];
        
        widthOfX = inOtherState.widthOfX;
        return *this;
    }
    
private:
    static inline uint32_t arraySize(const uint16_t inWidthOfX) {
        return 4 + inWidthOfX + inWidthOfX % 2 + inWidthOfX * inWidthOfX;
    }

    /**
     * @brief Rebind to a new storage array
     *
     * @param inWidthOfX If this value is positive, use it as the number of
     *     independent variables. This is needed during initialization, when
     *     the storage array is still all zero, but we do already know the
     *     with of the design matrix.
     *
     * @internal Array layout ():
     * - 0: numRows (number of rows seen so far)
     * - 1: widthOfX (number of coefficients)
     * - 2: y_sum (sum of independent variables seen so far)
     * - 3: y_square_sum (sum of squares of independent variables seen so far)
     * - 4: X_transp_Y (X^T y, for that parts of X and y seen so far)
     * - 4 + widthOfX + widthOfX % 2: (X^T X, as seen so far)
     *
     * Note that we want 16-byte alignment for all vectors and matrices. We
     * therefore ensure that X_transp_Y and X_transp_X begin at even positions.
     */
    void rebind(uint16_t inWidthOfX) {
        numRows.rebind(&mStorage[0]);
        widthOfX.rebind(&mStorage[1]);
        y_sum.rebind(&mStorage[2]);
        y_square_sum.rebind(&mStorage[3]);
        X_transp_Y.rebind(
            typename HandleTraits<Handle, LinAlgTypes>::TransparentHandle(&mStorage[4]),
            inWidthOfX);
        X_transp_X.rebind(
            typename HandleTraits<Handle, LinAlgTypes>::TransparentHandle(
                &mStorage[4 + inWidthOfX + (inWidthOfX % 2)]
            ), inWidthOfX, inWidthOfX);
    }

    Handle mStorage;

public:
    typename HandleTraits<Handle, LinAlgTypes>::ReferenceToUInt64 numRows;
    typename HandleTraits<Handle, LinAlgTypes>::ReferenceToUInt16 widthOfX;
    typename HandleTraits<Handle, LinAlgTypes>::ReferenceToDouble y_sum;
    typename HandleTraits<Handle, LinAlgTypes>::ReferenceToDouble y_square_sum;
    typename HandleTraits<Handle, LinAlgTypes>::ColumnVectorTransparentHandleMap X_transp_Y;
    typename HandleTraits<Handle, LinAlgTypes>::MatrixTransparentHandleMap X_transp_X;
};


/**
 * @brief Perform the linear-regression transition step
 * 
 * We update: the number of rows \f$ n \f$, the partial sums
 * \f$ \sum_{i=1}^n y_i \f$ and \f$ \sum_{i=1}^n y_i^2 \f$, the matrix
 * \f$ X^T X \f$, and the vector \f$ X^T \boldsymbol y \f$.
 */
AnyType
linregr_transition::run(AnyType &args) {
    // Arguments from SQL call. Immutable values passed by reference should be
    // instantiated from the respective <tt>_const</tt> class. Otherwise, the
    // abstraction layer will perform a deep copy (i.e., waste unnecessary
    // processor cycles).
    LinRegrTransitionState<MutableArrayHandle<double> > state = args[0];
    double y = args[1].getAs<double>();
    HandleMap<const ColumnVector> x = args[2].getAs<ArrayHandle<double> >();
    
    // See MADLIB-138. At least on certain platforms and with certain versions,
    // LAPACK will run into an infinite loop if pinv() is called for non-finite
    // matrices. We extend the check also to the dependent variables.
    if (!std::isfinite(y))
        throw std::invalid_argument("Dependent variables are not finite.");
    else if (!isfinite(x))
        throw std::invalid_argument("Design matrix is not finite.");
    
    // Now do the transition step.
    if (state.numRows == 0) {
        if (x.size() > std::numeric_limits<uint16_t>::max())
            throw std::domain_error("Number of independent variables cannot be "
                "larger than 65535.");
        
        state.initialize(*this, x.size());
    }
    state.numRows++;
    state.y_sum += y;
    state.y_square_sum += y * y;
    state.X_transp_Y.noalias() += x * y;
    state.X_transp_X.noalias() += x * trans(x);
    
    return state;
}

/**
 * @brief Perform the perliminary aggregation function: Merge transition states
 */
AnyType
linregr_merge_states::run(AnyType &args) {
    LinRegrTransitionState<MutableArrayHandle<double> > stateLeft = args[0];
    LinRegrTransitionState<ArrayHandle<double> > stateRight = args[1];
    
    // We first handle the trivial case where this function is called with one
    // of the states being the initial state
    if (stateLeft.numRows == 0)
        return stateRight;
    else if (stateRight.numRows == 0)
        return stateLeft;
    
    // Merge states together and return
    stateLeft += stateRight;
    return stateLeft;
}

/**
 * @brief Perform the linear-regression final step
 */
AnyType
linregr_final::run(AnyType &args) {
    const LinRegrTransitionState<ArrayHandle<double> > state = args[0];

    // See MADLIB-138. At least on certain platforms and with certain versions,
    // LAPACK will run into an infinite loop if pinv() is called for non-finite
    // matrices. We extend the check also to the dependent variables.
    if (!isfinite(state.X_transp_X) || !isfinite(state.X_transp_Y))
        throw std::invalid_argument("Design matrix is not finite.");
    
    SymmetricPositiveDefiniteEigenDecomposition<Matrix> decomposition(
        state.X_transp_X, EigenvaluesOnly, ComputePseudoInverse);
    
    // Precompute (X^T * X)^+
    Matrix inverse_of_X_transp_X = decomposition.pseudoInverse();

    // Vector of coefficients: For efficiency reasons, we want to return this
    // by reference, so we need to bind to db memory
    HandleMap<ColumnVector> coef(allocateArray<double>(state.widthOfX));
    coef.noalias() = inverse_of_X_transp_X * state.X_transp_Y;
    
    // explained sum of squares (regression sum of squares)
    double ess
        = dot(state.X_transp_Y, coef)
            - ((state.y_sum * state.y_sum) / state.numRows);

    // total sum of squares
    double tss
        = state.y_square_sum
            - ((state.y_sum * state.y_sum) / state.numRows);
    
    // With infinite precision, the following checks are pointless. But due to
    // floating-point arithmetic, this need not hold at this point.
    // Without a formal proof convincing us of the contrary, we should
    // anticipate that numerical peculiarities might occur.
    if (tss < 0)
        tss = 0;
    if (ess < 0)
        ess = 0;
    // Since we know tss with greater accuracy than ess, we do the following
    // sanity adjustment to ess:
    if (ess > tss)
        ess = tss;

    // coefficient of determination
    // If tss == 0, then the regression perfectly fits the data, so the
    // coefficient of determination is 1.
    double r2 = (tss == 0 ? 1 : ess / tss);

    // In the case of linear regression:
    // residual sum of squares (rss) = total sum of squares (tss) - explained
    // sum of squares (ess)
    // Proof: http://en.wikipedia.org/wiki/Sum_of_squares
    double rss = tss - ess;

    // Variance is also called the mean square error
	double variance = rss / (state.numRows - state.widthOfX);
    
    // Vector of standard errors and t-statistics: For efficiency reasons, we
    // want to return these by reference, so we need to bind to db memory
    HandleMap<ColumnVector> stdErr(allocateArray<double>(state.widthOfX));
    HandleMap<ColumnVector> tStats(allocateArray<double>(state.widthOfX));
    for (int i = 0; i < state.widthOfX; i++) {
        // In an abundance of caution, we see a tiny possibility that numerical
        // instabilities in the pinv operation can lead to negative values on
        // the main diagonal of even a SPD matrix
        if (inverse_of_X_transp_X(i,i) < 0) {
            stdErr(i) = 0;
        } else {
            stdErr(i) = std::sqrt( variance * inverse_of_X_transp_X(i,i) );
        }
        
        if (coef(i) == 0 && stdErr(i) == 0) {
            // In this special case, 0/0 should be interpreted as 0:
            // We know that 0 is the exact value for the coefficient, so
            // the t-value should be 0 (corresponding to a p-value of 1)
            tStats(i) = 0;
        } else {
            // If stdErr(i) == 0 then abs(tStats(i)) will be infinity, which is
            // what we need.
            tStats(i) = coef(i) / stdErr(i);
        }
    }
    
    // Vector of p-values: For efficiency reasons, we want to return this
    // by reference, so we need to bind to db memory
    HandleMap<ColumnVector> pValues(allocateArray<double>(state.widthOfX));
//    for (int i = 0; i < state.widthOfX; i++)
//        pValues(i) = 2. * (1. - studentT_cdf(
//                                    state.numRows - state.widthOfX,
//                                    std::fabs( tStats(i) )));
    
    // Return all coefficients, standard errors, etc. in a tuple
    AnyType tuple;
    tuple << coef << r2 << stdErr << tStats << pValues
        << decomposition.conditionNo();
    return tuple;
}

} // namespace regress

} // namespace modules

} // namespace madlib
