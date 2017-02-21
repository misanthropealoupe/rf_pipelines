import numpy as np
from numpy import random
import rf_pipelines


class mask_filler(rf_pipelines.py_wi_transform):
    """

    Modifies values in the intensity and weight arrays. If the weight is > w_cutoff, the weight is changed to 
    2.0 and left unmodified. Otherwise the weight is changed to 2.0 AND the intensity is replaced with 
    gaussian random noise with standard deviation given by a previously calculated variance array.

    Note that if the entire frequency channel was masked in the variance array (variance = 0), it will remain 
    masked. 

    Constructor Arguments
    ----------------------
    var_file - the .npy file containing the precalculated variance array

    n_varsamples - the number of samples used to calculate each variance datapoint and is the product of 
                   v1_chunk and v2_chunk n_varsamples * var.shape[1] must be equal to the number of samples
                   in the dataset being filled

    w_cutoff - weight cutoff above which the weight will not be replaced by random noise

    """

    def __init__(self, var_file, n_varsamples, w_cutoff, nt_chunk):
        name = "mask_filler(var_file='%s', w_cutoff=%d, nt_chunk=%d)" % (var_file, w_cutoff, nt_chunk)

        # Call base class constructor
        rf_pipelines.py_wi_transform.__init__(self, name)

        # Load the pre-calculated variance array, assumed to be in the same directory as the script
        self.var = np.load(var_file)
        
        # Initialize some other values
        self.n_varsamples = n_varsamples   # number of samples per data point in variance array
        self.w_cutoff = w_cutoff
        self.nt_postpad = 0
        self.nt_prepad = 0

        # FOR NOW nt_chunk must be a multiple of n_varsamples
        assert nt_chunk % n_varsamples == 0, \
            'For now, nt_chunk(=%d) must be a multiple of n_varsamples(=%d). I might implement some buffering later to fix this!' % (nt_chunk, n_varsamples)
        self.nt_chunk = nt_chunk


    def set_stream(self, s):
         self.nfreq = s.nfreq


    def start_substream(self, isubstream, t0):
        # Add some sort of buffer here later to remove need to nt_chunk % n_varsamples == 0 eventually
        pass


    def process_chunk(self, t0, t1, intensity, weights, pp_intensity, pp_weights):
        ivariance = 0
        imaxvar = self.var.shape[1]
        for frequency in xrange(self.nfreq):
            for i in xrange(intensity.shape[1]):
                if weights[frequency, i] > self.w_cutoff:
                    weights[frequency, i] = 2.0
                else:
                    sigma = (self.var[frequency, ivariance])**2
                    if sigma == 0:
                        weights[frequency, i] = 0
                    else:
                        intensity[frequency, i] = sigma * np.random.standard_normal()
                        weights[frequency, i] = 2.0
                if i % self.n_varsamples == 0:
                    ivariance += 1
                if i > imaxvar:
                    break
            ivariance = 0

    def end_substream(self):
        pass

