// Copyright (C) 2011 Torsten Albrecht and Ed Bueler and Constantine Khroulev
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include <cmath>
#include <cstring>
#include <petscda.h>
#include "iceModel.hh"
#include "Mask.hh"

// methods implementing PIK logic for -part_grid; see Albrecht et al 2011


//! Compute staggered grid velocities according to mask and regular grid velocities.
/*!
  In the finite volume interpretation, these are normal velocities at the faces
  of the cell.  The method avoids differencing velocities from ice free ocean locations.
*/
PetscErrorCode IceModel::cell_interface_velocities(bool do_part_grid,
                                                   int i, int j,
                                                   planeStar<PetscScalar> &vel) {
  PetscErrorCode ierr;
  IceModelVec2V *vel_advective;
  ierr = stress_balance->get_advective_2d_velocity(vel_advective); CHKERRQ(ierr);

  MaskQuery M(vMask);
  planeStar<PISMVector2> vreg = vel_advective->star(i, j);

  if (!do_part_grid) {
    // just compute (i, j) - centered "face" velocity components by average
    vel.e = 0.5 * (vreg.ij.u + vreg.e.u);
    vel.w = 0.5 * (vreg.w.u + vreg.ij.u);
    vel.n = 0.5 * (vreg.ij.v + vreg.n.v);
    vel.s = 0.5 * (vreg.s.v + vreg.ij.v);

    return 0;
  }

  if (M.icy(i, j) && (!M.ice_margin(i, j))) {
    // in the middle of ice or bedrock
    vel.e = 0.5 * (vreg.ij.u + vreg.e.u);
    vel.w = 0.5 * (vreg.w.u + vreg.ij.u);
    vel.n = 0.5 * (vreg.ij.v + vreg.n.v);
    vel.s = 0.5 * (vreg.s.v + vreg.ij.v);
  } else if (M.ice_margin(i, j)) {
    // on floating or grounded ice, but next to a ice-free grid cell
    vel.e = (M.ice_free(i + 1, j) ? vreg.ij.u : 0.5 * (vreg.ij.u + vreg.e.u));
    vel.w = (M.ice_free(i - 1, j) ? vreg.ij.u : 0.5 * (vreg.w.u + vreg.ij.u));
    vel.n = (M.ice_free(i, j + 1) ? vreg.ij.v : 0.5 * (vreg.ij.v + vreg.n.v));
    vel.s = (M.ice_free(i, j - 1) ? vreg.ij.v : 0.5 * (vreg.s.v + vreg.ij.v));
  } else if (M.next_to_floating_ice(i, j)) {
    // on an ice-free (or partially filled) cell next to an icy grid cell
    vel.e = (M.icy(i + 1, j) ? vreg.e.u : 0.0);
    vel.w = (M.icy(i - 1, j) ? vreg.w.u : 0.0);
    vel.n = (M.icy(i, j + 1) ? vreg.n.v : 0.0);
    vel.s = (M.icy(i, j - 1) ? vreg.s.v : 0.0);
  } else {
    // on ice-free ocean or land and no ice neighbors
    vel.e = 0.0;
    vel.w = 0.0;
    vel.n = 0.0;
    vel.s = 0.0;
  }

  return 0;
}


//! For ice-free (or partially-filled) cells adjacent to "full" floating cells, update Href.
/*!
  Should only be called if one of the neighbors is floating.

  FIXME: does not account for grounded tributaries: thin ice shelves may
  evolve from grounded tongue
*/
PetscReal IceModel::get_average_thickness(bool do_redist, planeStar<int> M, planeStar<PetscScalar> H) {

  // get mean ice thickness over adjacent floating ice shelf neighbors
  PetscReal H_average = 0.0;
  PetscInt N = 0;
  Mask m;

  if (m.floating_ice(M.e)) { H_average += H.e; N++; }
  if (m.floating_ice(M.w)) { H_average += H.w; N++; }
  if (m.floating_ice(M.n)) { H_average += H.n; N++; }
  if (m.floating_ice(M.s)) { H_average += H.s; N++; }

  if (N == 0) {
    SETERRQ(1, "N == 0;  call this only if a neighbor is floating!\n");
  }

  H_average = H_average / N;

  // reduces the guess at the front
  if (do_redist) {
    const PetscReal  mslope = 2.4511e-18*grid.dx / (300*600 / secpera);
    // for declining front C / Q0 according to analytical flowline profile in
    //   vandeveen with v0 = 300m / yr and H0 = 600m
    H_average -= 0.8*mslope*pow(H_average, 5);
  }

  return H_average;
}


//! Redistribute residual ice mass from subgrid-scale parameterization, when using -part_redist option.
/*!
  See [\ref Albrechtetal2011].  Manages the loop.

  FIXME: Reporting!

  FIXME: repeatRedist should be config flag?

  FIXME: resolve fixed number (=3) of loops issue
*/
PetscErrorCode IceModel::redistResiduals() {
  PetscErrorCode ierr;
  const PetscInt max_loopcount = 3;
  ierr = calculateRedistResiduals(); CHKERRQ(ierr); //while loop?

  for (int i = 0; i < max_loopcount && repeatRedist == PETSC_TRUE; ++i) {
    ierr = calculateRedistResiduals(); CHKERRQ(ierr); // sets repeatRedist
    ierr = verbPrintf(4, grid.com, "redistribution loopcount = %d\n", i); CHKERRQ(ierr);
  }
  return 0;
}


// This routine carries-over the ice mass when using -part_redist option, one step in the loop.
PetscErrorCode IceModel::calculateRedistResiduals() {
  PetscErrorCode ierr;
  ierr = verbPrintf(4, grid.com, "calculateRedistResiduals() is called\n"); CHKERRQ(ierr);

  IceModelVec2S vHnew = vWork2d[0];
  ierr = vH.copy_to(vHnew); CHKERRQ(ierr);
  ierr = vHnew.begin_access(); CHKERRQ(ierr);
  ierr = vH.begin_access(); CHKERRQ(ierr);

  ierr = vHref.begin_access(); CHKERRQ(ierr);
  ierr = vbed.begin_access(); CHKERRQ(ierr);

  IceModelVec2S vHresidualnew = vWork2d[1];
  ierr = vHresidual.copy_to(vHresidualnew); CHKERRQ(ierr);
  ierr = vHresidual.begin_access(); CHKERRQ(ierr);
  ierr = vHresidualnew.begin_access(); CHKERRQ(ierr);


  if (ocean == PETSC_NULL) { SETERRQ(1, "PISM ERROR: ocean == PETSC_NULL");  }
  PetscReal sea_level = 0.0; //FIXME
  ierr = ocean->sea_level_elevation(sea_level); CHKERRQ(ierr);

  PetscScalar minHRedist = 50.0; // to avoid the propagation of thin ice shelf tongues

  for (PetscInt i = grid.xs; i < grid.xs + grid.xm; ++i) {
    for (PetscInt j = grid.ys; j < grid.ys + grid.ym; ++j) {
      // first step: distributing residual ice masses
      if (vHresidual(i, j) > 0.0 && putOnTop==PETSC_FALSE) {
        
        planeStar<PetscScalar> thk = vH.star(i, j),
          bed = vbed.star(i, j);

        PetscInt N = 0; // counting empty / partially filled neighbors
        planeStar<bool> neighbors;
        neighbors.e = neighbors.w = neighbors.n = neighbors.s = false;

        // check for partially filled / empty grid cell neighbors (mask not updated yet, but vH is)
        if (thk.e == 0.0 && bed.e < sea_level) {N++; neighbors.e = true;}
        if (thk.w == 0.0 && bed.w < sea_level) {N++; neighbors.w = true;}
        if (thk.n == 0.0 && bed.n < sea_level) {N++; neighbors.n = true;}
        if (thk.s == 0.0 && bed.s < sea_level) {N++; neighbors.s = true;}

        //if (N > 0 && vH(i, j) > minHRedist)  {
		if (N > 0)  {
          //remainder ice mass will be redistributed equally to all adjacent
          //imfrac boxes (is there a more physical way?)
          if (neighbors.e) vHref(i + 1, j) += vHresidual(i, j) / N;
          if (neighbors.w) vHref(i - 1, j) += vHresidual(i, j) / N;
          if (neighbors.n) vHref(i, j + 1) += vHresidual(i, j) / N;
          if (neighbors.s) vHref(i, j - 1) += vHresidual(i, j) / N;

          vHresidualnew(i, j) = 0.0;
        } else {
          vHnew(i, j) += vHresidual(i, j); // mass conservation, but thick ice at one grid cell possible
          vHresidualnew(i, j) = 0.0;
          ierr = verbPrintf(4, grid.com, 
                            "!!! PISM WARNING: Hresidual has %d partially filled neighbors, "
                            " set ice thickness to vHnew = %.2e at %d, %d \n", 
                            N, vHnew(i, j), i, j ); CHKERRQ(ierr);
        }
      }
    }
  }
  ierr = vHnew.beginGhostComm(vH); CHKERRQ(ierr);
  ierr = vHnew.endGhostComm(vH); CHKERRQ(ierr);


  double  ocean_rho = config.get("sea_water_density"),
    ice_rho = config.get("ice_density"),
    C = ice_rho / ocean_rho;
  PetscScalar     H_average;
  PetscScalar     Hcut = 0.0;
  for (PetscInt i = grid.xs; i < grid.xs + grid.xm; ++i) {
    for (PetscInt j = grid.ys; j < grid.ys + grid.ym; ++j) {

      // second step: if neighbors which gained redistributed ice also become
      // full, this needs to be redistributed in a repeated loop
      if (vHref(i, j) > 0.0) {
        H_average = 0.0;
        PetscInt N = 0; // number of full floating ice neighbors (mask not yet updated)

        planeStar<PetscScalar> thk = vH.star(i, j),
          bed = vbed.star(i, j);

        if (thk.e > 0.0 && bed.e < sea_level - C * thk.e) { N++; H_average += thk.e; }
        if (thk.w > 0.0 && bed.w < sea_level - C * thk.w) { N++; H_average += thk.w; }
        if (thk.n > 0.0 && bed.n < sea_level - C * thk.n) { N++; H_average += thk.n; }
        if (thk.s > 0.0 && bed.s < sea_level - C * thk.s) { N++; H_average += thk.s; }

        if (N > 0){
          H_average = H_average / N;

          PetscScalar coverageRatio = vHref(i, j) / H_average;
          if (coverageRatio > 1.0) { // partially filled grid cell is considered to be full
            vHresidualnew(i, j) = vHref(i, j) - H_average;
            Hcut += vHresidualnew(i, j); // summed up to decide, if methods needs to be run once more
            vHnew(i, j) += H_average; //SMB?
            vHref(i, j) = 0.0;
          }
        } else { // no full floating ice neighbor
          vHnew(i, j) += vHref(i, j); // mass conservation, but thick ice at one grid cell possible
          vHref(i, j) = 0.0;
          vHresidualnew(i, j) = 0.0;
	      ierr = verbPrintf(4, grid.com, 
                            "!!! PISM WARNING: Hresidual=%.2f with %d partially filled neighbors, "
                            " set ice thickness to vHnew = %.2f at %d, %d \n", 
                            vHresidual(i, j), N , vHnew(i, j), i, j ); CHKERRQ(ierr);
        }
      }
    }
  }

  PetscScalar gHcut; //check, if redistribution should be run once more
  ierr = PetscGlobalSum(&Hcut, &gHcut, grid.com); CHKERRQ(ierr);
  putOnTop = PETSC_FALSE;
  if (gHcut > 0.0) { repeatRedist = PETSC_TRUE;
	if (gHcut < minHRedist) { putOnTop = PETSC_TRUE; }//to avoid a couple of repetition for the redistribution of very thin vHresiduals
  } else { repeatRedist = PETSC_FALSE;}
  //ierr = verbPrintf(3, grid.com, "!!! Hcut = %f \n", gHcut); CHKERRQ(ierr);

  ierr = vH.end_access(); CHKERRQ(ierr);
  ierr = vHnew.end_access(); CHKERRQ(ierr);
  // finally copy vHnew into vH and communicate ghosted values
  ierr = vHnew.beginGhostComm(vH); CHKERRQ(ierr);
  ierr = vHnew.endGhostComm(vH); CHKERRQ(ierr);

  ierr = vHref.end_access(); CHKERRQ(ierr);
  ierr = vbed.end_access(); CHKERRQ(ierr);

  ierr = vHresidual.end_access(); CHKERRQ(ierr);
  ierr = vHresidualnew.end_access(); CHKERRQ(ierr);
  ierr = vHresidualnew.beginGhostComm(vHresidual); CHKERRQ(ierr);
  ierr = vHresidualnew.endGhostComm(vHresidual); CHKERRQ(ierr);


  return 0;
}
