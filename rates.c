/* Calculation of rate coefficients. Mostly copied from CRaC */

/* 
 * Copyright (C) 2013 Evgeny Stambulchik
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <gsl/gsl_errno.h>
#include <gsl/gsl_integration.h>

#include <gsl/gsl_const_num.h>
#define ALPHA GSL_CONST_NUM_FINE_STRUCTURE

#include "cfacdb.h"

#define CFACDB_QAGI_EPS 1e-4

#define SQR(a) ((a)*(a))
#define CUBE(a) ((a)*(a)*(a))

typedef struct {
    unsigned int ndata; /* number of data points */
    double      *e;     /* energy grid           */
    double      *d;     /* data                  */

    double       ap0;   /* asymtote parameter #1 */
    double       ap1;   /* asymtote parameter #2 */

    double       d0;    /* threshold limit       */
    double       let;   /* low-e tangent         */

    int          cube;  /* cubic interpolation   */
} crac_intext_t;

typedef struct {
    double T;
    double de;
    int type;
    crac_intext_t intext;
} rate_int_params_t;

static double crac_intext(const crac_intext_t *intext, double x,
    double (*f_asymptote)(double x, double ap0, double ap1))
{
    double Omega = 0.0;
    unsigned int i, n;
    double *dx = intext->e, *dd = intext->d;

    n = intext->ndata;

    if (x < dx[0]) {
        /* low-e extrapolation */
        Omega = intext->d0 + intext->let*(x - 1.0);
    } else
    if (x <= dx[n - 1]) {
        /* interpolation */
        for (i = 0; i < n - 1; i++) {
            double x1 = dx[i], x2 = dx[i + 1];
            if (x <= x2) {
                double xavg = (x1 + x2)/2;
                double y1 = dd[i], y2 = dd[i + 1];
                double tang;
                
                if (intext->cube) {
                    y1 *= pow(x1, 3);
                    y2 *= pow(x2, 3);
                }
                
                tang = (y2 - y1)/(x2 - x1);
                
                if (x < xavg) {
                    Omega = y1 + tang*(x - x1);
                } else {
                    Omega = y2 - tang*(x2 - x);
                }
                
                if (intext->cube) {
                    Omega /= pow(x, 3);
                }
                
                break;
            }
        }
    } else {
        /* high-e extrapolation */
        double c = dx[n - 1]/x;
        double a_n = f_asymptote(dx[n - 1], intext->ap0, intext->ap1);
        if (a_n <= 0.0) {
            /* 1/e */
            Omega = dd[n - 1]*c;
        } else {
            double beta0 = dd[n - 1]/a_n;
            Omega = f_asymptote(x, intext->ap0, intext->ap1)*pow(beta0, c);
        }
    }
    
    return Omega;
}

static double get_e_MB_vf(double T, double E)
{
    return 2*sqrt(2/M_PI)*E/pow(T, 1.5)*exp(-E/T);
}

static double crac_born_asymptote(double x1, double b0, double b1)
{
    /* negative b0 corresponds to spin-forbidden transition */
    if (b0 > 0) {
        return b0*log(x1) + b1;
    } else {
        return b1;
    }
}

static double crac_rr_asymptote(double x1, double p0, double kl)
{
    return p0*pow(x1, -(3.5 + kl));
}

/* NB: rates include the degeneracy of the initial level!!! */
static double rate_int_f(double e, void *params) {
    const rate_int_params_t *p = params;
    double xs, Omega;
    
    double x = e/p->de, x1;
    
    switch (p->type) {
    case DB_SQL_CS_CE:
    case DB_SQL_CS_CI:
        x1 = x;
        Omega = crac_intext(&p->intext, x1, crac_born_asymptote);
        /* convert collisional strength to cross-section */
        xs = Omega*M_PI/(2.0*(x1*p->de));
        break;
    case DB_SQL_CS_PI:
        x1 = x + 1;
        Omega = crac_intext(&p->intext, x1, crac_rr_asymptote);
        /* d_gf/d_E* to RR cross-section */
        xs = Omega*(M_PI*CUBE(ALPHA))*p->de*SQR(x1)/x;
        break;
    default:
        return 0;
        break;
    }
    
    return xs*get_e_MB_vf(p->T, e);
}


static void crates_sink(const cfac_db_t *cdb,
    ctrans_cb_data_t *cbdata, void *udata)
{
    crates_cb_data_t rcbdata;
    crac_intext_t *intext;
    
    int gsl_status;
    double low_limit, split_limit, result, error;
    
    rate_int_params_t params;

    gsl_function F;

    double ratec = 0.0;
    
    struct {
        double T;
        cfac_db_crates_sink_t sink;
        void *udata;
        gsl_integration_workspace *w;
    } *rdata = udata;
    
    params.T    = rdata->T;
    params.de   = cbdata->de;
    params.type = cbdata->type;
    
    intext = &params.intext;
    
    intext->ndata = cbdata->nd;
    intext->e = cbdata->e;
    intext->d = cbdata->d;
    intext->ap0 = cbdata->ap0;
    intext->ap1 = cbdata->ap1;

    F.function = &rate_int_f;
    F.params   = &params;

    if (cbdata->type == DB_SQL_CS_CI) {
        /* ionization cross-sections approach 0 at threshold */
        intext->let = (intext->d[0])/(intext->e[0] - 1.0);
        intext->d0  = 0.0;
    } else {
        /* linear extrapolation */
        intext->let = (intext->d[1] - intext->d[0])/
                      (intext->e[1] - intext->e[0]);
        intext->d0  = intext->d[0] - intext->let*(intext->e[0] - 1.0);
        if (intext->d0 < 0.0) {
            intext->let = (intext->d[0])/(intext->e[0] - 1.0);
            intext->d0  = 0.0;
        }
    }
    
    if (cbdata->type == DB_SQL_CS_PI) {
        intext->cube = 1;
        low_limit   = 0.0;
    } else {
        intext->cube = 0;
        low_limit   = params.de;
    }

    split_limit = 3*params.de;
    
    gsl_status = gsl_integration_qags(&F, low_limit, split_limit,
        0, CFACDB_QAGI_EPS, 1000, rdata->w, &result, &error);
    if (gsl_status) {
        fprintf(stderr, "gsl_integration_qags() failed with %s\n",
            gsl_strerror(gsl_status));
        return;
    }
    ratec = result;
    
    gsl_status = gsl_integration_qagiu(&F, split_limit,
        0, CFACDB_QAGI_EPS, 1000, rdata->w, &result, &error);
    if (gsl_status) {
        fprintf(stderr, "gsl_integration_qagiu() failed with %s\n",
            gsl_strerror(gsl_status));
        return;
    }
    ratec += result;

    rcbdata.type  = cbdata->type;
    rcbdata.de    = cbdata->de;
    rcbdata.ii    = cbdata->ii;
    rcbdata.fi    = cbdata->fi;
    
    rcbdata.ratec = ratec;
    
    rdata->sink(cdb, &rcbdata, rdata->udata);
}

int cfac_db_crates(cfac_db_t *cdb, double T,
    cfac_db_crates_sink_t sink, void *udata)
{
    int rc;

    struct {
        double T;
        cfac_db_crates_sink_t sink;
        void *udata;
        gsl_integration_workspace *w;
    } rdata;
    
    rdata.sink  = sink;
    rdata.udata = udata;
    rdata.T = T;
    
    rdata.w = gsl_integration_workspace_alloc(1000);
    if (!rdata.w) {
        return 2;
    }
    
    rc = cfac_db_ctrans(cdb, crates_sink, &rdata);
    
    gsl_integration_workspace_free(rdata.w);
    
    return rc;
}