#include "../fd_tango.h"

#if FD_HAS_X86 && FD_HAS_DOUBLE

double
fd_tempo_tickcount_model( double * opt_tau ) {
  static double t0;
  static double tau;

  FD_ONCE_BEGIN {

    /* Assuming tickcount observes the tickcounter at a consistent point
       between when the call was made and when it returns, the
       difference between two adjacent calls is an estimate of the
       number of ticks required for a call.  We expect this difference
       to have a well defined minimum time with occassional delays due
       to various sources of CPU jitter.  The natural approach is to
       model call overhead then as a shifted exponential random
       variable.  To parameterize the model, we repeatedly measure how
       long a call takes.  The minimum of a bunch of IID samples is very
       fast converging for estimating the minimum if there is weird
       outliers on the negative side.  As such, we use a robust
       estimator to estimate the minimal overhead and jitter. */

    ulong iter = 0UL;
    for(;;) { 
#     define TRIAL_CNT 512UL
#     define TRIM_CNT  64UL
      double trial[ TRIAL_CNT ]; 
      for( ulong trial_idx=0UL; trial_idx<TRIAL_CNT; trial_idx++ ) {
        FD_COMPILER_MFENCE();
        long tic = fd_tickcount();
        FD_COMPILER_MFENCE();
        long toc = fd_tickcount();
        FD_COMPILER_MFENCE();
        trial[ trial_idx ] = (double)(toc - tic);
        FD_COMPILER_MFENCE();
      }
      double * sample     = trial + TRIM_CNT;
      ulong    sample_cnt = TRIAL_CNT - 2UL*TRIM_CNT;
      ulong    thresh     = sample_cnt >> 1;
      if( FD_LIKELY( fd_stat_robust_exp_fit_double( &t0, &tau, sample, sample_cnt, sample )>thresh ) && FD_LIKELY( t0>0. ) ) break;
#     undef TRIM_CNT
#     undef TRIAL_CNT
      iter++;
      if( iter==3UL ) {
        FD_LOG_WARNING(( "unable to model fd_tickcount() performance; using fallback and attempting to continue" ));
        t0 = 24.; tau = 4.;
        break;
      }
    }

  } FD_ONCE_END;

  if( opt_tau ) opt_tau[0] = tau;
  return t0;
}

double
fd_tempo_wallclock_model( double * opt_tau ) {
  static double t0;
  static double tau;

  FD_ONCE_BEGIN {

    /* This is the same as the above but for the wallclock */

    ulong iter = 0UL;
    for(;;) { 
#     define TRIAL_CNT 1024UL
#     define TRIM_CNT  128UL
      double trial[ TRIAL_CNT ]; 
      for( ulong trial_idx=0UL; trial_idx<TRIAL_CNT; trial_idx++ ) {
        FD_COMPILER_MFENCE();
        long tic = fd_log_wallclock();
        FD_COMPILER_MFENCE();
        long toc = fd_log_wallclock();
        FD_COMPILER_MFENCE();
        trial[ trial_idx ] = (double)(toc - tic);
        FD_COMPILER_MFENCE();
      }
      double * sample     = trial + TRIM_CNT;
      ulong    sample_cnt = TRIAL_CNT - 2UL*TRIM_CNT;
      ulong    thresh     = sample_cnt >> 1;
      if( FD_LIKELY( fd_stat_robust_exp_fit_double( &t0, &tau, sample, sample_cnt, sample )>thresh ) && FD_LIKELY( t0>0. ) ) break;
#     undef TRIM_CNT
#     undef TRIAL_CNT
      iter++;
      if( iter==3UL ) {
        FD_LOG_WARNING(( "unable to model fd_log_wallclock() performance; using fallback and attempting to continue" ));
        t0 = 27.; tau = 1.;
        break;
      }
    }

  } FD_ONCE_END;

  if( opt_tau ) opt_tau[0] = tau;
  return t0;
}

double
fd_tempo_tick_per_ns( double * opt_sigma ) {
  static double mu;
  static double sigma;

  FD_ONCE_BEGIN {

    /* We measure repeatedly how much the tickcount and wallclock change
       over the same approximately constant time interval.  We do a pair
       observations to minimize errors in computing the interval (note
       that any remaining jitters should be zero mean such that they
       should statistically cancel in the rate calculation).  We use a
       robust estimate to get the avg and rms in the face of random
       sources of noise, assuming the sample distribution is reasonably
       well modeled as normal. */

    ulong iter = 0UL;
    for(;;) { 
#     define TRIAL_CNT 32UL
#     define TRIM_CNT   4UL
      double trial[ TRIAL_CNT ]; 
      for( ulong trial_idx=0UL; trial_idx<TRIAL_CNT; trial_idx++ ) {
        long then; long toc; fd_tempo_observe_pair( &then, &toc );
        fd_log_sleep( 16777216L ); /* ~16.8 ms */
        long now; long tic; fd_tempo_observe_pair( &now, &tic );
        trial[ trial_idx ] = (double)(tic-toc) / (double)(now-then);
      }
      double * sample     = trial + TRIM_CNT;
      ulong    sample_cnt = TRIAL_CNT - 2UL*TRIM_CNT;
      ulong    thresh     = sample_cnt >> 1;
      if( FD_LIKELY( fd_stat_robust_norm_fit_double( &mu, &sigma, sample, sample_cnt, sample )>thresh ) && FD_LIKELY( mu>0. ) )
        break;
#     undef TRIM_CNT
#     undef TRIAL_CNT
      iter++;
      if( iter==3UL ) {
        FD_LOG_WARNING(( "unable to measure tick_per_ns accurately; using fallback and attempting to continue" ));
        mu = 3.; sigma = 0.;
        break;
      }
    }

  } FD_ONCE_END;

  if( opt_sigma ) opt_sigma[0] = sigma;
  return mu;
}

long
fd_tempo_observe_pair( long * opt_now,
                       long * opt_tic ) {
  long best_wc;
  long best_tc;
  long best_jt;

  do {

    /* Do an alternating series of:

         tickcount
         wallclock
         tickcount
         wallclock
         tickcount
         ...
         wallclock
         tickcount

       observations and pick the wallclock observation that had the
       smallest elapsed time between adjacent tickcount observations.

       Since the wallclock / tickcounter returns a monotonically
       non-decreasing observation of the wallclock / tickcount at a
       point in time between when the call was made and when it
       returned, we know that this wallclock observation is the one we
       made that we know best when it was made in the tickcount stream.
       Further, we have lower and upper bounds of the value of the
       tickcounter in this read.  We start the alternation with the
       tickcount because that is typically the lower overhead, more
       deterministic one and less likely to get jerked around behind our
       back.
       
       Theoretically, this exploits how the minimum of a shifted
       exponential random variable converges.  Since the time to read
       the various clocks is expected to be reasonably modeled as a
       shifted exponential random variable, it doesn't take many trials
       to get something close to the minimum (estimating the minimum of
       a shifted exponential random variable takes way fewer samples and
       is way more accurate than say the estimating the average of a
       normally distributed random variable). */

#   define TRIAL_CNT (4) /* 1 "warmup", 4 real reads */

    long wc[ TRIAL_CNT+1 ];
    long tc[ TRIAL_CNT+1 ];
    FD_COMPILER_MFENCE();
    tc[0] = fd_tickcount();
    FD_COMPILER_MFENCE();
    for( ulong trial_idx=0UL; trial_idx<TRIAL_CNT; trial_idx++ ) {
      wc[ trial_idx+1UL ] = fd_log_wallclock();
      FD_COMPILER_MFENCE();
      tc[ trial_idx+1UL ] = fd_tickcount();
      FD_COMPILER_MFENCE();
    }

    best_wc = wc[1];
    best_tc = tc[1];
    best_jt = best_tc - tc[0];
    for( ulong trial_idx=1UL; trial_idx<TRIAL_CNT; trial_idx++ ) {
      long wci = wc[ trial_idx+1UL ];
      long tci = tc[ trial_idx+1UL ];
      long jti = tci - tc[ trial_idx ];
      int  c   = (jti<=best_jt);
      best_wc  = fd_long_if( c, wci, best_wc );
      best_tc  = fd_long_if( c, tci, best_tc );
      best_jt  = fd_long_if( c, jti, best_jt );
    }

#   undef TRIAL_CNT

  } while(0);

  if( FD_UNLIKELY( best_jt<0L ) ) { /* paranoia */
    FD_LOG_WARNING(( "fd_tickcount() does not appear to be monotonic; joint read may not be accurate; attempting to continue" ));
    best_jt = 0L;
  }

  if( opt_now ) opt_now[0] = best_wc;
  if( opt_tic ) opt_tic[0] = best_tc - (best_jt>>1); /* Use lower and upper bound midpoint (could be improved statistically) */
  return best_jt;
}

#endif
