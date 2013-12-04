/*
 * Polynomial based WENO scheme construction routines for non-uniform
 * grids.
 *
 * These routines use C polynomial routines to compute the
 * finite-volume reconstruction polynomials p(x) for each cell i and
 * left shift r.  The polynomials are stored in monomial form.
 *
 * One we have these polynomials, we can compute reconstruction
 * coefficients c[i,l,r,j] (given reconstruction points $\xi \in
 * [-1,1]$) and smoothness coefficients beta[i,r,m,n].
 *
 * Note: based upon some preliminary timing, if one unrolled all of
 * the loops in poly.c (for a specific avalue of k), one could get
 * about an order of magnitude speedup compared to the generic
 * routines as implemented.
 *
 * :Author: Matthew Emmett
 */

#define PY_ARRAY_UNIQUE_SYMBOL weno_nonuniform_ARRAY_API

#include <Python.h>
#include <numpy/ndarrayobject.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nfweno.h"

/*
 * Compute p(x) <- (x + a) p(x) inplace.
 */
#define inplace_poly_mult_xpa(p1, a)			\
  for (int z=p1.n; z>0; z--)				\
    p1.p[z] = (p1.p[z-1] + (a) * p1.p[z]);		\
  p1.p[0] = (a) * p1.p[0];				\

/*
 * Compute p1(x) <- p1(x) + p2(x) inplace.
 */
#define inplace_poly_add(p1, p2)			\
  for (int z=0; z<=p1.n; z++) p1.p[z] += p2.p[z];

/*
 * Compute p1(x) <- a * p1(x) inplace.
 */
#define inplace_poly_scale(p1, a)		        \
  for (int z=0; z<=p1.n; z++) p1.p[z] *= a;

/*
 * Compute the k-order reconstruction polynomials c[i,r,j] for each
 * valid center cell i, left shift r, and cell j given the cell
 * boundaries x.
 */
weno weno_recon_polys(const weno w0)
{
  weno w = w0;

  const int il = w.k-1;
  dtype xlocal[2*w.k];

  for (int i=w.k-1; i<w.nc-(w.k-1); i++) {

    // center each polynomial about the left edge of cell
    /* XXX: Matt: @Ben, does this look right to you?  I'm trying to
     *            center things at the left edge of the cell.
     */
    for (int j=0; j<2*w.k; j++)
      xlocal[j] = w.x[i-il+j] - w.x[i];

    for (int r=0; r<w.k; r++) {
      for (int j=0; j<w.k; j++) {
	const int irj = i*w.k*w.k + r*w.k + j;

	w.c_irj_x[irj] = poly_zero(w.k-1);
	for (int l=j+1; l<=w.k; l++) {
	  poly sum_m = poly_zero(w.k-1);
	  for (int m=0; m<=w.k; m++) {
	    if (m==l) continue;
	    poly prd_n = poly_one(w.k-1);
	    for (int n=0; n<=w.k; n++) {
	      if ((n==l) || (n==m)) continue;
	      inplace_poly_mult_xpa(prd_n, -xlocal[il-r+n]);
	    }
	    inplace_poly_add(sum_m, prd_n);
	  }
	  dtype a = 1.0;
	  for (int m=0; m<=w.k; m++) {
	    if (m==l) continue;
	    a *= (xlocal[il-r+l] - xlocal[il-r+m]);
	  }
	  a = 1.0/a;
	  inplace_poly_scale(sum_m, a);
	  inplace_poly_add(w.c_irj_x[irj], sum_m);
	} // for l

      } // for j
    } // for r
  } // for i

  return w;
}

/*
 * Compute reconstruction coefficients from the (already computed)
 * reconstruction polynomials.
 */
weno weno_recon_coefs(const weno w0)
{
  weno w = w0;

  for (int i=w.k-1; i<w.nc-(w.k-1); i++)
    for (int r=0; r<w.k; r++)
      for (int j=0; j<w.k; j++)
	for (int n=0; n<w.nxi; n++) {
	  const dtype xi   = 0.5 * (1.0 + w.xi[n]) * (w.x[i+1] - w.x[i]);
	  const int   ilrj = w.csi*i + w.csl*n + w.csr*r + w.csj*j;
	  const int   irj  = i*w.k*w.k + r*w.k + j;
	  w.c_ilrj[ilrj] = poly_eval(w.c_irj_x[irj], xi);
	}

  return w;
}

/*
 * Compute smoothness coefficients from the (already computed)
 * reconstruction polynomials.
 */
weno weno_smoothness_coefs(const weno w0)
{
  weno w = w0;

  for (int i=w.k-1; i<w.nc-(w.k-1); i++) {
    const dtype dx = w.x[i+1] - w.x[i];
    for (int r=0; r<w.k; r++) {
      for (int m=0; m<w.k; m++) {
	const int irm = i*w.k*w.k + r*w.k + m;
	for (int n=m; n<w.k; n++) {
	  const int irn  = i*w.k*w.k + r*w.k + n;

	  int  multi = m == n ? 1 : 2;
	  poly pm    = w.c_irj_x[irm];
	  poly pn    = w.c_irj_x[irn];

	  dtype l2 = 0.0;
	  for (int d=1; d<w.k; d++) {
	    pm = poly_diff(pm, 1);
	    pn = poly_diff(pn, 1);
	    l2 += multi * pow(dx, 2*d-1) * poly_int(poly_mult(pm, pn), 0.0, dx);
	  }

	  w.beta[w.bsi*i + w.bsr*r + w.bsm*m + w.bsn*n] = l2;
	}
      }
    }
  }

  return w;
}

PyObject *py_nonuniform_coeffs(PyObject *self, PyObject *args)
{
  PyArrayObject *x_py, *xi_py, *c_py, *beta_py;
  double *x, *xi, *c, *beta;
  int k;

  /* parse options */
  if (!PyArg_ParseTuple (args, "iOOOO", &k, &xi_py, &x_py, &c_py, &beta_py))
    return NULL;

  if (x_py->nd != 1 || x_py->descr->type_num != PyArray_DOUBLE) {
    PyErr_SetString(PyExc_ValueError, "x must be one-dimensional and of type double");
    return NULL;
  }

  if (xi_py->nd != 1 || xi_py->descr->type_num != PyArray_DOUBLE) {
    PyErr_SetString(PyExc_ValueError, "xi must be one-dimensional and of type double");
    return NULL;
  }

  if (c_py->nd != 4 || c_py->descr->type_num != PyArray_DOUBLE) {
    PyErr_SetString(PyExc_ValueError, "c must be four-dimensional and of type double");
    return NULL;
  }

  if (beta_py->nd != 4 || beta_py->descr->type_num != PyArray_DOUBLE) {
    PyErr_SetString(PyExc_ValueError, "beta must be four-dimensional and of type double");
    return NULL;
  }

  x    = (double *) PyArray_DATA(x_py);
  xi   = (double *) PyArray_DATA(xi_py);
  c    = (double *) PyArray_DATA(c_py);
  beta = (double *) PyArray_DATA(beta_py);

  int nx = PyArray_DIM(x_py, 0);
  int nxi = PyArray_DIM(xi_py, 0);

  weno w;
  w.k       = k;
  w.nx      = nx;
  w.nc      = nx - 1;
  w.nxi     = nxi;
  w.x       = x;
  w.xi      = xi;
  w.c_irj_x = calloc(sizeof(poly), w.nc*w.k*w.k);
  w.c_ilrj  = c;
  w.beta    = beta;

  w.csi = c_py->strides[0] / sizeof(double);
  w.csl = c_py->strides[1] / sizeof(double);
  w.csr = c_py->strides[2] / sizeof(double);
  w.csj = c_py->strides[3] / sizeof(double);

  w.bsi = beta_py->strides[0] / sizeof(double);
  w.bsr = beta_py->strides[1] / sizeof(double);
  w.bsm = beta_py->strides[2] / sizeof(double);
  w.bsn = beta_py->strides[3] / sizeof(double);

  w = weno_recon_polys(w);
  w = weno_recon_coefs(w);
  w = weno_smoothness_coefs(w);

  free(w.c_irj_x);

  Py_INCREF (Py_None);
  return Py_None;
}


static PyMethodDef cnonuniformmethods[] = {
  {"nonuniform_coeffs", py_nonuniform_coeffs, METH_VARARGS, ""},
  {NULL, NULL, 0, NULL}
};


PyObject *py_nonuniform_weno_coeffs(PyObject *self, PyObject *args);
PyMODINIT_FUNC
initcnonuniform (void)
{
  (void) Py_InitModule("cnonuniform", cnonuniformmethods);
  import_array ();
}