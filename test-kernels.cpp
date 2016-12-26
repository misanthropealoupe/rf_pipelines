// Note: this file should only include kernel headers (kernels/*.hpp), not toplevel headers (./*.hpp)
// (This is because it has a Makefile dependency on the former but not the latter.)

#include <cassert>
#include "simd_helpers/simd_debug.hpp"
#include "kernels/clip2d.hpp"

using namespace std;
using namespace rf_pipelines;


// -------------------------------------------------------------------------------------------------
//
// General-purpose helpers


// Cut-and-pasted from rf_pipelines_internals.hpp, since we don't want to #include it.
template<typename T>
inline T *aligned_alloc(size_t nelts)
{
    if (nelts == 0)
	return NULL;

    // align to 64-byte cache lines
    void *p = NULL;
    if (posix_memalign(&p, 64, nelts * sizeof(T)) != 0)
	throw std::runtime_error("couldn't allocate memory");

    memset(p, 0, nelts * sizeof(T));
    return reinterpret_cast<T *> (p);
}


// Frequently used in conjunction with simd_helpers::vectorize()
template<typename T> 
inline double maxabs(const std::vector<T> &v)
{
    assert(v.size() > 0);

    double ret = fabs(v[0]);
    for (unsigned int i = 1; i < v.size(); i++)
	ret = max(ret, fabs(double(v[i])));

    return ret;
}


// -------------------------------------------------------------------------------------------------


struct random_chunk {
    const int nfreq;
    const int nt;
    const int stride;
    
    float *intensity = nullptr;
    float *weights = nullptr;

    random_chunk(std::mt19937 &rng, int nfreq, int nt, int stride);
    random_chunk(std::mt19937 &rng, int nfreq, int nt);
    ~random_chunk();
    
    // noncopyable
    random_chunk(const random_chunk &) = delete;
    random_chunk &operator=(const random_chunk &) = delete;
};


random_chunk::random_chunk(std::mt19937 &rng, int nfreq_, int nt_, int stride_) :
    nfreq(nfreq_), nt(nt_), stride(stride_)
{
    assert(nfreq > 0);
    assert(nt > 0);
    assert(stride >= nt);

    intensity = aligned_alloc<float> (nfreq * stride);
    weights = aligned_alloc<float> (nfreq * stride);

    std::normal_distribution<> gdist;

    for (int i = 0; i < nfreq * stride; i++) {
	intensity[i] = gdist(rng) + 1.0;
	weights[i] = std::uniform_real_distribution<>()(rng);
    }
}


random_chunk::random_chunk(std::mt19937 &rng, int nfreq_, int nt_) :
    random_chunk(rng, nfreq_, nt_, nt_ + std::uniform_int_distribution<>(0,4)(rng))
{ }


random_chunk::~random_chunk()
{
    free(intensity);
    free(weights);
    intensity = weights = nullptr;
}


// -------------------------------------------------------------------------------------------------


template<typename T>
static void reference_clip2d_wrms(T &mean, T &rms, const T *intensity, const T *weights, int nfreq, int nt, int stride, int nds_f, int nds_t)
{
    assert(nfreq % nds_f == 0);
    assert(nt % nds_t == 0);

    // double-precision here
    double acc0 = 0.0;
    double acc1 = 0.0;
    double acc2 = 0.0;
    
    for (int ifreq = 0; ifreq < nfreq; ifreq += nds_f) {
	for (int it = 0; it < nt; it += nds_t) {
	    T ival = 0.0;
	    T wval = 0.0;

	    for (int jfreq = ifreq; jfreq < ifreq + nds_f; jfreq++) {
		for (int jt = it; jt < it + nds_t; jt++) {
		    ival += intensity[jfreq*stride + jt];
		    wval += weights[jfreq*stride + jt];
		}
	    }

	    acc0 += double(wval);
	    acc1 += double(wval) * double(ival);
	    acc2 += double(wval) * double(ival) * double(ival);
	}
    }

    // FIXME case of invalid entries not tested
    mean = acc1/acc0;
    rms = sqrt(acc2/acc0 - mean*mean);
}


template<typename T, unsigned int S, unsigned int Df, unsigned int Dt>
static void test_kernel_clip2d_wrms(std::mt19937 &rng, int nfreq, int nt, int stride)
{
    assert(nfreq % Df == 0);
    assert(nt % (Dt*S) == 0);
    assert(stride >= nt);

    random_chunk rc(rng, nfreq, nt, stride);
    
    simd_t<T,S> mean, rms;
    _kernel_clip2d_wrms<T,S,Df,Dt> (mean, rms, rc.intensity, rc.weights, nfreq, nt, rc.stride);

    float ref_mean, ref_rms;
    reference_clip2d_wrms(ref_mean, ref_rms, rc.intensity, rc.weights, nfreq, nt, rc.stride, Df, Dt);

    vector<float> delta1 = vectorize(mean - simd_t<T,S> (ref_mean));
    vector<float> delta2 = vectorize(rms - simd_t<T,S> (ref_rms));    

    if ((maxabs(delta1) > 1.0e-3 * Df*Dt) || (maxabs(delta2) > 1.0e-3 * sqrt(Df*Dt))) {
	cerr << "test_kernel_clip2d_wrms failed: S=" << S << ", Df=" << Df << ", Dt=" << Dt 
	     << ", nfreq=" << nfreq << ", nt=" << nt << ", stride=" << stride << "\n"
	     << "  mean: " << ref_mean << ", " << mean << "\n"
	     << "  rms: " << ref_rms << ", " << rms << "\n";
	exit(1);
    }
}


template<typename T, unsigned int S, unsigned int Df, unsigned int Dt>
static void test_kernel_clip2d_wrms(std::mt19937 &rng)
{
    int nfreq = Df * std::uniform_int_distribution<>(10,20)(rng);
    int nt = Dt * S * std::uniform_int_distribution<>(10,20)(rng);
    int stride = nt + std::uniform_int_distribution<>(0,4)(rng);

    test_kernel_clip2d_wrms<T,S,Df,Dt> (rng, nfreq, nt, stride);
}


// Fixed Df, many Dt
template<typename T, unsigned int S, unsigned int Df, unsigned int MaxDt, typename std::enable_if<(MaxDt==1),int>::type = 0>
static void test_kernel_clip2d_wrms_varying_dt(std::mt19937 &rng)
{
    test_kernel_clip2d_wrms<T,S,Df,1> (rng);
}

// Fixed Df, many Dt
template<typename T, unsigned int S, unsigned int Df, unsigned int MaxDt, typename std::enable_if<(MaxDt>1),int>::type = 0>
static void test_kernel_clip2d_wrms_varying_dt(std::mt19937 &rng)
{
    test_kernel_clip2d_wrms_varying_dt<T,S,Df,MaxDt/2> (rng);
    test_kernel_clip2d_wrms<T,S,Df,MaxDt> (rng);
}

// Many Df, many Dt
template<typename T, unsigned int S, unsigned int MaxDf, unsigned int MaxDt, typename std::enable_if<(MaxDf==1),int>::type = 0>
static void test_kernel_clip2d_wrms_all(std::mt19937 &rng)
{
    test_kernel_clip2d_wrms_varying_dt<T,S,1,MaxDt> (rng);
}

// Many Df, many Dt
template<typename T, unsigned int S, unsigned int MaxDf, unsigned int MaxDt, typename std::enable_if<(MaxDf>1),int>::type = 0>
static void test_kernel_clip2d_wrms_all(std::mt19937 &rng)
{
    test_kernel_clip2d_wrms_all<T,S,MaxDf/2,MaxDt> (rng);
    test_kernel_clip2d_wrms_varying_dt<T,S,MaxDf,MaxDt> (rng);
}


// -------------------------------------------------------------------------------------------------


int main(int argc, char **argv)
{
    std::random_device rd;
    std::mt19937 rng(rd());

    test_kernel_clip2d_wrms_all<float,8,32,32> (rng);

    return 0;
}