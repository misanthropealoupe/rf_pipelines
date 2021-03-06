#include "rf_pipelines.hpp"

namespace rf_pipelines {
#if 0
}; // pacify emacs c-mode
#endif

/**
 This is a pair of wi_transform classes, Saver and Reverter, where
 Saver saves the chunks, and Reverter restores them.  A transform
 chain can have a Saver, some other transform that modifies the
 stream, and then a Reverter, which well restore the stream to its
 original values.  Multiple Reverters can read from one Saver, but
 this requires exposing the classes rather than using opaque
 wi_transform objects, hence this header file.
 */

std::pair<std::shared_ptr<rf_pipelines::wi_transform>,
          std::shared_ptr<rf_pipelines::wi_transform> >
make_reverter(ssize_t nt_chunk);

class Reverter;

class Saver : public wi_transform {
    friend class Reverter;
public:
    Saver(ssize_t nt_chunk);
    virtual ~Saver();
    virtual void start_substream(int isubstream, double t0);
    virtual void end_substream();
    virtual void set_stream(const rf_pipelines::wi_stream &stream);
    virtual void process_chunk(double t0, double t1,
                               float* ii, float* ww, ssize_t stride,
                               float* pp_ii, float* pp_ww, ssize_t pp_stride);

protected:
    float* intensity;
    float* weight;

    double t0;
    double t1;

    void revert_chunk(double t0, double t1, float* ii, float* ww, ssize_t stride);

};

class Reverter : public wi_transform {
public:
    Reverter(std::shared_ptr<Saver> s);
    virtual ~Reverter();
    virtual void set_stream(const wi_stream &stream);
    virtual void start_substream(int isubstream, double t0);
    virtual void end_substream();
    virtual void process_chunk(double t0, double t1,
                               float* ii, float* ww, ssize_t stride,
                               float* pp_ii, float* pp_ww, ssize_t pp_stride);

protected:
    std::shared_ptr<Saver> saver;
};

std::shared_ptr<Saver> make_saver(ssize_t nt_chunk);

}
