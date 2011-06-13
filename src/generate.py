'''Generate variou WENO routines in C.  These are wrapped and
accessible through :func:`pyweno.weno.weno`.'''


import pyweno.points
import pyweno.symbolic
import pyweno.kernels
import pyweno.wrappers

K = range(3, 5)
generate = [ 'left',
             'right',
             'gauss_legendre',
             'gauss_lobatto',
             'gauss_radau' ]


for k in K:

  print 'generating k:', k

  kernel  = pyweno.kernels.KernelGenerator('c')
  wrapper = pyweno.wrappers.WrapperGenerator(kernel)

  beta = pyweno.symbolic.jiang_shu_smoothness_coefficients(k)
  kernel.set_smoothness(beta)

  base = 'smoothness%03d' % k
  with open(base + '.c', 'w') as f:
    f.write(wrapper.smoothness(base))

  for g in generate:

    print '  generating:', g

    if g == 'left':
      xi = [ -1 ]
    elif g == 'right':
      xi = [ 1 ]
    else:
      func = getattr(pyweno.points, g)
      xi   = func(k)

    (varpi, split) = pyweno.symbolic.optimal_weights(k, xi)
    coeffs = pyweno.symbolic.reconstruction_coefficients(k, xi)

    kernel.set_optimal_weights(varpi, split)
    kernel.set_reconstruction_coefficients(coeffs)

    base = g + '%03d' % k 
    with open(base + '.c', 'w') as f:
      f.write(wrapper.reconstruction(base, local_weights=True, wrapper=True))