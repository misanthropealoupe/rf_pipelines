import numpy as np
import numpy.random

import rf_pipelines


class adversarial_masker(rf_pipelines.py_wi_transform):
    """
    A half-finished transform, intended to stress-test the online variance estimation logic
    by masking a bunch of rectangle-shaped regions of the input array.  If the online variance
    estimation logic is working well, then the trigger plots shouldn't show any false positives!

    The constructor argument 'nt_reset' is not very well thought-out, but is vaguely intended
    to be the timescale (expressed as a number of samples) over which the variance estimation
    logic resets itself.

    TODO: right now, this isn't very "adversarial", since all it does is mask a few large
    rectangles which correspond to timestream gaps of different sizes.  Let's try to test
    as many weird cases as possible!
    """

    def __init__(self, nt_chunk=1024, nt_reset=65536):
        rf_pipelines.py_wi_transform.__init__(self, 'adversarial_masker(nt_chunk=%d, nt_reset=%d)' % (nt_chunk, nt_reset))
        self.nt_prepad = 0
        self.nt_postpad = 0
        self.nt_chunk = nt_chunk
        self.nt_reset = nt_reset


    def set_stream(self, s):
        self.nfreq = s.nfreq

        # Initialize:
        #   self.rectangles: list of (freq_lo, freq_hi, t_lo, t_hi) quadruples to be masked.
        #   self.nt_max: nominal timestream size, expressed as number of samples.
        #   self.nt_processed: samples processed so far (this counter is incremented in process_chunk())
        
        self.rectangles = [ ]
        self.nt_max = self.nt_reset 
        self.nt_processed = 0

        # We place timestream gaps of a few different sizes, separated by 'nt_reset' samples.
        for i in xrange(8):
            nt_rect = self.nt_reset // 2**i    # gap size
            self.rectangles += [ (0, self.nfreq, self.nt_max, self.nt_max + nt_rect) ]
            self.nt_max = self.nt_max + nt_rect + self.nt_reset

        # Debug
        # for rect in self.rectangles:
        #     print rect

    
    def process_chunk(self, t0, t1, intensity, weights, pp_intensity, pp_weights):
        # Loop over rectangles
        for (ifreq0, ifreq1, it0, it1) in self.rectangles:
            # Range of indices in current chunk which overlap with the rectangle
            it0 = max(0, min(self.nt_chunk, it0 - self.nt_processed))
            it1 = max(0, min(self.nt_chunk, it1 - self.nt_processed))

            # If there is an overlap, then mask (by setting weights to zero)
            if it0 < it1:
                weights[ifreq0:ifreq1,it0:it1] = 0.0

        self.nt_processed += self.nt_chunk