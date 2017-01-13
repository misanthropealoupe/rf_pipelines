import numpy as np
import rf_pipelines

def filter_stdv(intensity, weights, thr=3, axis=1, dsample_nfreq=None, dsample_nt=None, imitate_cpp=False):
    """Helper function for std_dev_filter. Modifies 'weights' array in place."""
    
    (nfreq, nt_chunk) = intensity.shape
    
    # ------ Helper '__init__' calls ------
    assert (axis == 0 or axis == 1), "axis must be 0 (along freq; constant time), or 1 (along time; constant freq)."
    assert thr >= 1., "threshold must be >= 1."
    assert nt_chunk > 0
    assert (dsample_nt is None or dsample_nt > 0), "Invalid downsampling number along the time axis!"
    assert (dsample_nfreq is None or dsample_nfreq > 0), "Invalid downsampling number along the freq axis!"

    # ------ Helper 'set_stream' calls ------
    coarse_grained = (dsample_nfreq < nfreq) or (dsample_nt < nt_chunk)

    if dsample_nfreq is None:
        dsample_nfreq = nfreq
    if dsample_nt is None:
        dsample_nt = nt_chunk

    if nfreq % dsample_nfreq != 0:
        raise RuntimeError("filter_stdv: current implementation requires 'dsample_nfreq' to be a divisor of stream nfreq.")
    if nt_chunk % dsample_nt != 0:
        raise RuntimeError("filter_stdv: current implementation requires 'dsample_nt' to be a divisor of 'nt_chunk'.")
    
    # ------ Helper 'process_chunk' calls ------
    # Let's make a ref to the original high-resolution weights.
    weights_hres = weights

    if coarse_grained:
        # Downsample the weights and intensity.
        (intensity, weights) = rf_pipelines.wi_downsample(intensity, weights, dsample_nfreq, dsample_nt)

    # Compute the weighted standard deviation of the intensity
    # array along the selected axis.  We also build up a boolean
    # mask 'sd_mask' which is False where 'sd' is invalid.

    if imitate_cpp:
        den = np.sum(weights, axis=axis)
        sd_mask = (den > 0)

        # In the C++ code, we use the variance rather than the standard deviation
        # (i.e. no square root), and subtract the mean.
        den = np.where(sd_mask, den, 1.0)
        mean = np.sum(weights*intensity, axis=axis) / den
        mean_e = rf_pipelines.tile_arr(mean, axis, dsample_nfreq, dsample_nt)
        sd = np.sum(weights*(intensity-mean_e)**2, axis=axis) / den

        # Expand the mask to exclude elements where the standard deviation
        # is very small compared to the mean.
        sd_mask = np.logical_and(sd_mask, (sd > (1.0e-10 * mean**2)))

    else:
        num = np.asarray(np.sum(weights*(intensity)**2, axis=axis))
        den = np.asarray(np.sum(weights, axis=axis))
        np.putmask(den, den==0., 1.0)
        sd = np.sqrt(num/den)
        sd_mask = np.ones_like(sd)

    # Tile 'sd' so that it matches with the shape of intensity.
    sd = rf_pipelines.tile_arr(sd, axis, dsample_nfreq, dsample_nt)
    sd_mask = rf_pipelines.tile_arr(sd_mask, axis, dsample_nfreq, dsample_nt)

    # This block of code creates a mask, and hence filters,
    # based on the mean and stdv of 'sd' along THE OTHER axis.
    axis = np.abs(axis-1)
    den = np.sum(sd_mask, axis=axis)

    # Avoid divide-by-zero [explain]
    den = np.maximum(den, 1)

    sd_mean0 = np.sum(sd_mask * sd, axis=axis) / den
    sd_mean = rf_pipelines.tile_arr(sd_mean0, axis, dsample_nfreq, dsample_nt)

    # Regulate [explain]
    sd_var = np.sum(sd_mask * (sd-sd_mean)**2, axis=axis) / den
    sd_var = np.where(sd_var > 1.0e-10 * sd_mean0**2, sd_var, 0.0)
    sd_stdv = rf_pipelines.tile_arr(sd_var**0.5, axis, dsample_nfreq, dsample_nt)

    assert sd.shape == sd_mean.shape == sd_stdv.shape

    # Boolean array which is True for masked values
    mask = np.abs(sd-sd_mean) >= (thr*sd_stdv)

    if coarse_grained:
        mask = rf_pipelines.upsample(mask, nfreq, nt_chunk)

    np.putmask(weights_hres, mask, 0.)

class std_dev_filter(rf_pipelines.py_wi_transform):
    """
   Masks weights array based on the weighted (intensity) 
   standard deviation deviating by some sigma. 
   
    Constructor syntax:

      t = std_dev_filter(thr=3., axis=None, nt_chunk=1024)

      'thr=3.' is the sigma value to clip. 

      'axis=0' is the axis convention:
        0: along freq; constant time.
        1: along time; constant freq.

      'nt_chunk=1024' is the buffer size.
      
      'dsample_nfreq' and 'dsample_nt' are the downsampled
       number of pixles along the freq and time axes, respectively.
    """

    def __init__(self, thr=3., axis=None, nt_chunk=1024, dsample_nfreq=None, dsample_nt=None):
        
        self.thr = thr
        self.axis = axis
        self.nt_chunk  = nt_chunk
        self.nt_prepad = 0
        self.nt_postpad = 0
        self.dsample_nfreq = dsample_nfreq
        self.dsample_nt = dsample_nt

        name = 'std_dev_filter(thr=%f, axis=%s, nt_chunk=%d' % (thr, axis, nt_chunk)
        if dsample_nfreq is not None:
            name += ', dsample_nfreq=%d' % dsample_nfreq
        if dsample_nt is not None:
            name += ', dsample_nt=%d' % dsample_nt
        name += ')'
        self.name = name

    def set_stream(self,stream):
        self.nfreq = stream.nfreq

    def process_chunk(self, t0, t1, intensity, weights, pp_intensity, pp_weights):
        filter_stdv(intensity, weights, self.thr, self.axis, self.dsample_nfreq, self.dsample_nt)
