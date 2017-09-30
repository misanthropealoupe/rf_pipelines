#include <fstream>
#include <algorithm>
#include "rf_pipelines_internals.hpp"

using namespace std;

namespace rf_pipelines {
#if 0
}  // emacs pacifier
#endif


// Global registry (class_name -> json_constructor).
//
// Note: We use a pointer here, rather than simply declaring a static global unordered_map<...>, 
// because we initialize entries with other static global declarations (see example at the end
// of pipeline.cpp), and there is no way to ensure that the unordered_map<> constructor call
// occurs first.  
//
// In constrast, if we use a pointer, then the first call to register_json_constructor() will initialize 
// the regsistry (see code below).  However, this means that all users of the registry (e.g. from_json(),
// _show_registered_json_constructors()) must handle the corner case where no constructors have been
// registered yet, and the pointer is still NULL.

using json_registry_t = unordered_map<string, pipeline_object::json_constructor_t>;
static json_registry_t *json_registry = nullptr;   // global


pipeline_object::pipeline_object(const string &name_) : 
    name(name_) 
{ }


void pipeline_object::_throw(const string &msg) const
{
    string prefix = (name.size() > 0) ? ("rf_pipelines: " + name + ": ") : "rf_pipelines: ";
    throw runtime_error(prefix + msg);
}


// -------------------------------------------------------------------------------------------------
//
// bind() and friends.


void pipeline_object::bind()
{
    if (is_bound())
	return;

    ssize_t n = this->get_preferred_chunk_size();
    if (n <= 0)
	_throw("this object cannot be first in pipeline");

    ring_buffer_dict rb_dict;
    Json::Value json_data;

    this->bind(rb_dict, n, n, json_data);

    // Note: currently throwing away 'json_data' after bind() completes.
    // Should it be saved somewhere?
}

// The non-virtual function bind() wraps the pure virtual function _bind().
void pipeline_object::bind(ring_buffer_dict &rb_dict, ssize_t nt_chunk_in_, ssize_t nt_maxlag_, Json::Value &json_data)
{    
    rf_assert(nt_chunk_in_ > 0);
    rf_assert(nt_maxlag_ > 0);

    if (name.size() == 0)
	throw runtime_error("rf_pipelines: pipeline_object did not initialize 'name' field in its constructor");

    if (is_bound())
	_throw("Double call to pipeline_object::bind().  This can happen if a pipeline_object is reused in a pipeline.");

    this->nt_chunk_in = nt_chunk_in_;
    this->nt_maxlag = nt_maxlag_;
    
    this->_bind(rb_dict, json_data);

    rf_assert(nt_chunk_in == nt_chunk_in_);
    rf_assert(nt_maxlag == nt_maxlag_);

    if (nt_maxgap < 0)
	_throw("_bind() failed to initialize nt_maxgap");
    if (nt_chunk_out <= 0)
	_throw("_bind() failed to initialize nt_chunk_out");
    if (nt_contig <= 0)
	_throw("_bind() failed to initialize nt_contig");

    for (auto &rb: this->all_ring_buffers)
	rb->update_params(nt_contig, nt_maxlag + nt_maxgap);
}


shared_ptr<ring_buffer> pipeline_object::get_buffer(ring_buffer_dict &rb_dict, const string &key)
{
    if (!has_key(rb_dict, key))
	_throw("buffer '" + key + "' does not exist in pipeline");

    auto ret = rb_dict[key];
    all_ring_buffers.push_back(ret);

    return ret;
}


shared_ptr<ring_buffer> pipeline_object::create_buffer(ring_buffer_dict &rb_dict, const string &key, const vector<ssize_t> &cdims, ssize_t nds)
{
    if (has_key(rb_dict, key))
	_throw("buffer '" + key + "' already exists in pipeline");

    auto ret = make_shared<ring_buffer> (cdims, nds);

    rb_dict[key] = ret;
    all_ring_buffers.push_back(ret);
    new_ring_buffers.push_back(ret);
    
    return ret;
}


bool pipeline_object::is_bound() const
{
    return (nt_chunk_in > 0);
}


// Default virtual (see comment in rf_pipelines.hpp for discussion)
ssize_t pipeline_object::get_preferred_chunk_size()
{
    return 0;
}


// -------------------------------------------------------------------------------------------------
//
// allocate(), deallocate()


void pipeline_object::allocate()
{
    if (!is_bound())
	this->bind();
    
    for (auto &p: this->new_ring_buffers)
	p->allocate();

    this->_allocate();
}



void pipeline_object::deallocate()
{
    this->_deallocate();
    
    for (auto &p: this->new_ring_buffers)
	p->deallocate();
}


// Default virtuals do nothing.
void pipeline_object::_allocate() { }
void pipeline_object::_deallocate() { }
    

// -------------------------------------------------------------------------------------------------
//
// run() and friends


Json::Value pipeline_object::run(const string &outdir, int verbosity, bool clobber)
{
    if (this->out_mp)
	throw runtime_error("rf_pipelines: 'out_mp' is set in pipeline_object::run(), maybe you are rerunning pipeline after throwing an exception?");

    auto mp = make_shared<outdir_manager> (outdir, clobber);
    Json::Value j_in(Json::objectValue);

    // Note: allocate() calls bind() if necessary.
    this->allocate();
    this->start_pipeline(mp, j_in);

    // We wrap the advance() loop in try..except, so that if an exception is thrown, we
    // still call end_pipeline() to clean up, and write partially complete output files.

    bool exception_thrown = false;
    string exception_text;

    try {
	ssize_t nt_end = SSIZE_MAX;
    
	while (this->pos_lo < nt_end) {
	    ssize_t m = pos_hi + nt_chunk_in;
	    ssize_t n = this->advance(m, m);
	    nt_end = min(nt_end, n);
	}
    } catch (std::exception &e) {
	exception_text = e.what();
	exception_thrown = true;
	// fall through...
    }

    // Note: end_pipeline() clears outdir_manager, plot_groups.
    Json::Value j_out(Json::objectValue);
    this->end_pipeline(j_out);

    // Try to write json file, even if exception was thrown.
    if (outdir.size() > 0) {
	string json_filename = outdir + "/rf_pipeline_0.json";
	ofstream f(json_filename);

	if (f.fail())
	    _throw("couldn't open output file " + json_filename);

	Json::StyledWriter w;
	f << w.write(j_out);

	if (verbosity >= 2)
	    cout << "wrote " << json_filename << endl;
    }

    // Note: no call to deallocate().
    // FIXME add boolean flag to deallocate on pipeline exit.

    // FIXME is there a better way to save and re-throw the exception?
    // The naive approach, saving a copy of the std::exception, doesn't preserve the exception_text.

    if (exception_thrown)
	throw runtime_error(exception_text);

    return j_out;
}


// The non-virtual function advance() wraps the pure virtual function _advance().
ssize_t pipeline_object::advance(ssize_t pos_hi_, ssize_t pos_max_)
{
    struct timeval tv0 = get_time();

    rf_assert(nt_chunk_in > 0);
    rf_assert(nt_chunk_out > 0);
    
    rf_assert(pos_hi <= pos_hi_);
    rf_assert(pos_hi_ <= pos_max_);
    rf_assert(pos_max_ <= pos_hi + nt_maxlag);
    rf_assert(pos_hi_ % nt_chunk_in == 0);

    this->pos_hi = pos_hi_;
    this->pos_max = pos_max_;    

    ssize_t ret = this->_advance();

    if (pos_hi != pos_hi_)
	_throw("internal error: value of pos_hi was modified in advance()");
    if (pos_lo % nt_chunk_out)
	_throw("internal error: pos_lo is not a multiple of nt_chunk_out after advance()");	
    if (pos_lo > pos_hi)
	_throw("internal error: pos_lo > pos_hi after advance()");
    if (pos_hi - pos_lo > nt_maxgap)
	_throw("internal error: (pos_hi-pos_lo) > nt_maxgap after advance().");

    this->time_spent_in_transform += time_diff(tv0, get_time());

    return ret;
}


void pipeline_object::start_pipeline(const shared_ptr<outdir_manager> &mp, Json::Value &j)
{
    if (this->out_mp)
	throw runtime_error("rf_pipelines: either double call to start_pipeline() without calling end_pipeline(), or pipeline_object appears twice in pipeline");

    this->out_mp = mp;
    this->plot_groups.clear();
    this->time_spent_in_transform = 0.0;
    
    this->pos_lo = 0;
    this->pos_hi = 0;
    this->pos_max = 0;
    
    for (auto &p: this->new_ring_buffers)
	p->start();

    this->_start_pipeline(j);
}


// This wrapper is a little silly, but consistent with our general naming conventions!
void pipeline_object::end_pipeline(Json::Value &j)
{
    if (!j.isObject())
	_throw("end_pipeline(): internal error: Json::Value was not an Object as expected");

    // FIXME should there be a ring_buffer::end()?
    this->_end_pipeline(j);

    if (!j.isMember("name"))
	j["name"] = this->name;
    if (!j.isMember("cpu_time"))
	j["cpu_time"] = this->time_spent_in_transform;

    if (!j.isMember("plots") && (plot_groups.size() > 0)) {
	for (const auto &g: plot_groups) {
	    if (g.is_empty)
		continue;
	    
	    Json::Value jp;
            jp["name"] = g.name;
            jp["nt_per_pix"] = g.nt_per_pix;
            jp["ny"] = g.ny;
            jp["it0"] = Json::Value::Int64(g.curr_it0);
            jp["it1"] = Json::Value::Int64(g.curr_it1);
            jp["files"].append(g.files);

            j["plots"].append(jp);
	}
    }
    
    this->out_mp.reset();
    this->plot_groups.clear();
}


// Default virtuals
void pipeline_object::_start_pipeline(Json::Value &j) { }
void pipeline_object::_end_pipeline(Json::Value &j) { }


// -------------------------------------------------------------------------------------------------
//
// json serialization/deserialization


// default virtual
Json::Value pipeline_object::jsonize() const
{
    _throw("jsonize() not implemented");
    return Json::Value();  // compiler pacifier
}


// static member function
void pipeline_object::register_json_constructor(const string &class_name, const json_constructor_t &f)
{
    if (class_name.size() == 0)
	throw runtime_error("rf_pipelines::pipeline_object::register_json_constructor(): class_name must be a nonempty string");

    // First call to register_json_constructor() will initialize the regsistry.
    if (!json_registry)
	json_registry = new json_registry_t;

    auto p = json_registry->find(class_name);

    if (p != json_registry->end())
	throw runtime_error("rf_pipelines::pipeline_object::register_json_constructor(): duplicate registration for class_name='" + class_name + "'");

    (*json_registry)[class_name] = f;
}


// Static member function, for debugging.
void pipeline_object::_show_registered_json_constructors()
{
    // All users of the json registry must handle the corner case where no constructors have
    // been registered yet, and the pointer is still NULL,

    if (!json_registry)
	return;

    vector<string> all_class_names;

    for (const auto &p: *json_registry)
	all_class_names.push_back(p.first);

    std::sort(all_class_names.begin(), all_class_names.end());
    
    cout << "[";
    for (const string &class_name : all_class_names)
	cout << " " << class_name;
    cout << " ]";
}


// static member function
pipeline_object::json_constructor_t pipeline_object::_find_json_constructor(const string &class_name)
{
    // All users of the json registry must handle the corner case where no constructors have
    // been registered yet, and the pointer is still NULL,

    if (!json_registry)
	return NULL;

    auto p = json_registry->find(class_name);
    return (p != json_registry->end()) ? p->second : NULL;
}


// static member function
shared_ptr<pipeline_object> pipeline_object::from_json(const Json::Value &x)
{
    if (!x.isObject())
	throw runtime_error("rf_pipelines: pipeline_object::from_json(): expected json argument to be an Object");

    // throws exception if 'class_name' not found
    string class_name = string_from_json(x, "class_name");

    json_constructor_t f = _find_json_constructor(class_name);

    if (f == NULL)
	throw runtime_error("rf_pipelines::pipeline_object::from_json(): class_name='" + class_name + "' not found, maybe you're missing a call to pipeline_object::from_json_converter()?");

    shared_ptr<pipeline_object> ret = f(x);

    if (!ret)
	throw runtime_error("rf_pipelines::pipeline_object::from_json(): json_constructor for class_name='" + class_name + "' returned empty pointer");

    return ret;
}


// -------------------------------------------------------------------------------------------------
//
// Output file management (including plots)


// Returns group id
int pipeline_object::add_plot_group(const string &name, int nt_per_pix, int ny)
{
    if (nt_per_pix < 1)
	_throw("add_plot_group(): nt_per_pix must be >= 1");
    if (ny < 1)
	_throw("add_plot_group(): ny must be >= 1");

    for (const auto &p: this->plot_groups)
	if (p.name == name)
	    _throw("add_plot_group(): duplicate plot_group_name '" + name + "'");

    plot_group g;
    g.name = name;
    g.nt_per_pix = nt_per_pix;
    g.ny = ny;

    this->plot_groups.push_back(g);
    return plot_groups.size()-1;
}


string pipeline_object::add_plot(const string &basename, int64_t it0, int nt, int nx, int ny, int group_id)
{
    if (this->plot_groups.size() == 0)
	_throw("add_plot() called but not plot_groups defined, maybe you forgot to call add_plot_group()?");

    if ((group_id < 0) || (group_id >= (int)plot_groups.size()))
	_throw("add_plot(): bad group_id specified");

    plot_group &g = plot_groups[group_id];

    if (nt != g.nt_per_pix * nx)
	_throw("add_plot(): requirement (nt == nx*nt_per_pix) failed");
    if (ny != g.ny)
	_throw("add_plot(): ny doesn't match value specified in add_plot_group()");

    if (g.is_empty) {
	g.is_empty = false;
	g.curr_it0 = it0;
    }
    else if (it0 != g.curr_it1)
	_throw("add_plot(): plot time ranges are not contiguous");

    string filename = this->add_file(basename);

    Json::Value file;
    file["filename"] = basename;
    file["it0"] = Json::Value::Int64(it0);
    file["nx"] = nx;

    g.curr_it1 = it0 + nt;
    g.files.append(file);

    return filename;
}


string pipeline_object::add_file(const string &basename)
{
    if (!out_mp)
	_throw("internal error: no outdir_manager in pipeline_object::add_file()");
    if (out_mp->outdir.size() == 0)
	_throw("attempted to write output file, but outdir='' (or python None) was specified in run()");

    return this->out_mp->add_file(basename);
}


}  // namespace rf_pipelines