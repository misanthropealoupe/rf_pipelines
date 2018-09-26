#include "rf_pipelines_internals.hpp"
#include "rf_pipelines_inventory.hpp"
#include "ch_frb_io.hpp"

using namespace std;

namespace rf_pipelines {
#if 0
}; // pacify emacs c-mode
#endif

chime_mask_counter::chime_mask_counter(int nt_chunk_, string where_) :
    mask_counter_transform(nt_chunk_, where_, "chime_mask_counter")
{}

void chime_mask_counter::set_stream(std::shared_ptr<ch_frb_io::intensity_network_stream> _stream,
                                    int _beam) {
    stream = _stream;
    beam = _beam;
}

void chime_mask_counter::_process_chunk(float *intensity, ssize_t istride, float *weights, ssize_t wstride, ssize_t pos)
{
    int nt = nt_chunk/nds;

    if (!stream) {
        cout << "chime_mask_counter: processing chunk, but stream not set" << endl;
        mask_counter_transform::_process_chunk(intensity, istride, weights, wstride, pos);
        return;
    }

    if (!fpga_counts_initialized)
	throw runtime_error("rf_pipelines::chime_mask_counter internal error: fpga count field were not initialized as expected");
    
    cout << "chime_mask_counter: finding chunk for pos " << pos << endl;

    uint64_t fpga_counts = pos * fpga_counts_per_sample + initial_fpga_count;
    //cout << "FPGA counts: " << fpga_counts << endl;

    shared_ptr<ch_frb_io::assembled_chunk> chunk = stream->find_assembled_chunk(beam, fpga_counts);
    bool good = true;
    if (!chunk) {
        cout << "Could not find a chunk for beam " << beam << ", FPGA counts " << fpga_counts << endl;
        good = false;
    }
    if (good && !chunk->rfi_mask) {
        cout << "Found chunk, but it has no rfi_mask array" << endl;
        good = false;
    }
    if (good && (chunk->nrfifreq != nfreq)) {
        cout << "Chunk expected number of RFI frequencies " << chunk->nrfifreq
             << " but RFI mask has " << nfreq << endl;
        good = false;
    }
    if (!good) {
        mask_counter_transform::_process_chunk(intensity, istride, weights, wstride, pos);
        return;
    }

    mask_counter_measurements meas;
    meas.pos = pos;
    meas.nsamples = nfreq*nt;
    meas.nsamples_masked = 0;
    meas.nt = nt;
    meas.nt_masked = 0;
    meas.nf = nfreq;
    meas.nf_masked = 0;
    meas.freqs_masked = shared_ptr<uint16_t>((uint16_t*)calloc(nfreq, sizeof(uint16_t)), free);
    meas.times_masked = shared_ptr<uint16_t>((uint16_t*)calloc(nt,    sizeof(uint16_t)), free);

    uint16_t* fm = meas.freqs_masked.get();
    uint16_t* tm = meas.times_masked.get();

    uint8_t* rfimask = chunk->rfi_mask;

    for (int i_f=0; i_f<nfreq; i_f++) {
        for (int i_t=0; i_t<nt/8; i_t++) {
            uint8_t m_out = 0;
            for (int j=0; j<8; j++) {
                if (weights[i_f*wstride + 8*i_t + j] == 0) {
                    meas.nsamples_masked++;
                    fm[i_f]++;
                    tm[8*i_t+j]++;
                } else
                    m_out |= (1 << j);
            }
            rfimask[i_f*nt/8 + i_t] = m_out;
        }
    }

    chunk->has_rfi_mask = true;

    for (int i_f=0; i_f<nfreq; i_f++)
        if (fm[i_f] == nt)
            meas.nf_masked++;
    for (int i_t=0; i_t<nt; i_t++)
        if (tm[i_t] == nfreq)
            meas.nt_masked++;
            
    cout << "chime_mask_counter " << where << ", pos " << pos << ": N samples masked: " << meas.nsamples_masked << "/" << (meas.nsamples) << "; n times " << meas.nt_masked << "/" << meas.nt << "; n freqs " << meas.nf_masked << "/" << meas.nf << endl;
    for (const auto &cb : callbacks)
        cb->mask_count(meas);

    // Notify stream's output_devices that a chunk has had its
    // rfi_mask filled in.
    for (auto od : stream->ini_params.output_devices)
        od->filled_rfi_mask(chunk);
}


void chime_mask_counter::_start_pipeline(Json::Value &j)
{
    this->initial_fpga_count = uint64_t_from_json(j, "initial_fpga_count");
    this->fpga_counts_per_sample = int_from_json(j, "fpga_counts_per_sample");
    this->fpga_counts_initialized = true;
}


Json::Value chime_mask_counter::jsonize() const
    {
        Json::Value ret;

        ret["class_name"] = "chime_mask_counter";
        ret["nt_chunk"] = int(this->get_prebind_nt_chunk());
        ret["where"] = where;
        return ret;
    }


shared_ptr<chime_mask_counter>
chime_mask_counter::from_json(const Json::Value &j)
    {
        ssize_t nt_chunk = ssize_t_from_json(j, "nt_chunk");
        string where = string_from_json(j, "where");
        return make_shared<chime_mask_counter> (nt_chunk, where);
    }

namespace {
    struct _init {
        _init() {
            pipeline_object::register_json_deserializer("chime_mask_counter", chime_mask_counter::from_json);
        }
    } init;
}

// Externally callable
shared_ptr<wi_transform> make_chime_mask_counter(int nt_chunk, string where)
{
    return make_shared<chime_mask_counter> (nt_chunk, where);
}


}  // namespace rf_pipelines



