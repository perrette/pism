#! /usr/bin/env python
#
# Copyright (C) 2011 Ed Bueler and Constantine Khroulev and David Maxwell
# 
# This file is part of PISM.
# 
# PISM is free software; you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version.
# 
# PISM is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
# 
# You should have received a copy of the GNU General Public License
# along with PISM; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA


import sys, petsc4py
petsc4py.init(sys.argv)
from petsc4py import PETSc

import PISM

class testj(PISM.ssa.SSAExactTestCase):
  def initGrid(self,Mx,My):
    halfWidth = 300.0e3
    Lx = halfWidth; Ly = halfWidth
    PISM.util.init_shallow_grid(self.grid,Lx,Ly,Mx,My,PISM.XY_PERIODIC);

  def initPhysics(self):
    config = self.config
    self.basal = PISM.IceBasalResistancePlasticLaw(
           config.get("plastic_regularization") / PISM.secpera,
           config.get_flag("do_pseudo_plastic_till"),
           config.get("pseudo_plastic_q"),
           config.get("pseudo_plastic_uthreshold") / PISM.secpera);

    self.ice = PISM.CustomGlenIce(self.grid.com, "", config)
    self.enthalpyconverter = PISM.EnthalpyConverter(config)

  def initSSACoefficients(self):
    solver = self.solver
    solver.allocateBCs()

    solver.tauc.set(0.0) # irrelevant for test J
    solver.bed.set(0.0) 
    solver.ice_mask.set(PISM.MASK_FLOATING)
    solver.bc_mask.set(0) # No dirichlet data.

    solver.enthalpy.set(528668.35); # arbitrary; corresponds to 263.15 Kelvin at depth=0.

    ocean_rho = self.config.get("sea_water_density");
    
    nu0 = 30.0 * 1.0e6 * PISM.secpera; # 9.45e14 Pa s 
    H0 = 500.0;                        # 500 m typical thickness

    # The PISM.utils.Access object ensures that we call beginAccess for each
    # variable in 'vars', and that endAccess is called for each one on exiting
    # the 'with' block.
    vars = [solver.thickness, solver.surface, solver.bc_mask, solver.vel_bc]
    with PISM.util.Access(vars):
      grid = self.grid
      for (i,j) in grid.points():
        x = grid.x[i]; y = grid.y[j]
        (H,junk,u,v) = PISM.exactJ(x,y);
        solver.thickness[i,j] = H;
        solver.surface[i,j] = (1.0 - self.ice.rho / ocean_rho) * H; #// FIXME task #7297
  
        # // special case at center point (Dirichlet BC)
        if (i == (grid.Mx)/2) and (j == (grid.My)/2):
          solver.bc_mask[i,j] = 1;
          solver.vel_bc[i,j] = [u,v]

    # // communicate what we have set
    for v in vars:
      v.beginGhostComm(); v.endGhostComm();

    # Test J has a viscosity that is independent of velocity.  So we force a 
    # constant viscosity by settting the strength_extension
    # thickness larger than the given ice thickness. (max = 770m).

    solver.ssa.strength_extension.set_notional_strength(nu0 * H0);
    solver.ssa.strength_extension.set_min_thickness(800.);


  def exactSolution(self,i,j,x,y):
    (j1,j2,u,v) = PISM.exactJ(x,y);
    return [u,v]


# The main code for a run follows:
if __name__ == '__main__':
  context = PISM.Context()

  for o in PISM.OptionsGroup(context.com,"","Test J"):
    Mx = PISM.optionsInt("-Mx","Number of grid points in x-direction",default=61)
    My = PISM.optionsInt("-My","Number of grid points in y-direction",default=61)
    output_file = PISM.optionsString("-o","output file",default="testj.nc")
    verbosity = PISM.optionsInt("-verbose","verbosity level",default=3)

  PISM.setVerbosityLevel(verbosity)
  tc = testj(Mx,My)
  tc.solve()
  tc.report()
  tc.write(output_file)
