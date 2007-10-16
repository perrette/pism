// Copyright (C) 2004-2007 Jed Brown and Ed Bueler
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
#include "pism_signal.h"

PetscInt verbosityLevel;

// following numerical values have some significance; see updateSurfaceElevationAndMask() below
const int IceModel::MASK_SHEET = 1;
const int IceModel::MASK_DRAGGING = 2;
const int IceModel::MASK_FLOATING = 3;
// (modMask(mask[i][j]) == MASK_FLOATING) is criteria for floating; ..._OCEAN0 only used if -ocean_kill 
const int IceModel::MASK_FLOATING_OCEAN0 = 7;

PetscErrorCode getFlowLawFromUser(MPI_Comm com, IceType* &ice, PetscInt &flowLawNum) {
    PetscErrorCode ierr;
    PetscTruth     flowlawSet = PETSC_FALSE, useGK = PETSC_FALSE;

    ierr = PetscOptionsGetInt(PETSC_NULL, "-law", &flowLawNum, &flowlawSet); CHKERRQ(ierr);
    ierr = PetscOptionsHasName(PETSC_NULL, "-gk", &useGK); CHKERRQ(ierr);
    if (useGK==PETSC_TRUE) {
      flowlawSet = PETSC_TRUE;
      flowLawNum = 4;
    }

    ierr = verbPrintf((flowlawSet == PETSC_TRUE) ? 3 : 5,com, 
        "  [using flow law %d (where 0=Paterson-Budd,1=cold P-B,2=warm P-B,3=Hooke,4=Goldsby-Kohlstedt)]\n",
        flowLawNum); CHKERRQ(ierr);
    
    switch (flowLawNum) {
      case 0: // Paterson-Budd
        ice = new ThermoGlenIce;  
        break;
      case 1: // cold part of P-B
        ice = new ThermoGlenArrIce;  
        break;
      case 2: // warm part of P-B
        ice = new ThermoGlenArrIceWarm;  
        break;
      case 3: // Hooke
        ice = new ThermoGlenIceHooke;
        break;
      case 4: // Goldsby Kohlstedt
        ice = new HybridIce;  
        break;
      case 5: // Goldsby Kohlstedt stripped down
        ice = new HybridIceStripped;  
        break;
      default:
        SETERRQ(1,"\nflow law number for to initialize IceModel must be 0,1,2,3,4,5\n");
    }
    return 0;
}


IceModel::IceModel(IceGrid &g, IceType &i): grid(g), ice(i) {
  PetscErrorCode ierr;

  pism_signal = 0;
  signal(SIGTERM, pism_signal_handler);
  signal(SIGUSR1, pism_signal_handler);

  createBasal_done = PETSC_FALSE;
  top0ctx_created = PETSC_FALSE;
  createVecs_done = PETSC_FALSE;

  for (PetscInt nn = 0; nn < tnN; nn++) {
    runtimeViewers[nn] = PETSC_NULL;
  }
  createViewers_done = PETSC_FALSE;

  ierr = setDefaults();
  if (ierr != 0) {
    verbPrintf(1,grid.com, "Error setting defaults.\n");
    PetscEnd();
  }
  
  psParams.svlfp = 0.0;  // default polar stereographic projection settings
  psParams.lopo = 90.0;
  psParams.sp = -71.0;
}


IceModel::~IceModel() {
  if (createVecs_done == PETSC_TRUE) {
    destroyVecs();
  }
  if (createViewers_done == PETSC_TRUE) {
    destroyViewers();
  }
  if (createBasal_done == PETSC_TRUE) {
    delete basal;
  }
}


//! Allocate all Vecs defined in IceModel.
/*! Initialization of an IceModel is confusing.  Here is a description of the intended order:
	\li 1. The constructor for IceModel.  Note IceModel has a member "grid", of class IceGrid. 
	   Note grid.p, an IceParam, is also constructed; this sets 
	   defaults for (grid.p->)Mx,My,Mz,Mbz,Lx,Ly,Lz,Lbz,dx,dy,dz,year.
	\li [1.5] derivedClass::setFromOptions() to get options special to derived class
	\li 2. setFromOptions() to get all options *including* Mx,My,Mz,Mbz
	\li [2.5] initFromFile_netCDF() which reads Mx,My,Mz,Mbz from file and overwrites previous; if 
	   this represents a change the user is warned
	\li 3. createDA(), which uses only Mx,My,Mz,Mbz
	\li 4. createVecs() uses DA to create/allocate Vecs
	\li [4.5] derivedClass:createVecs() to create/allocate Vecs special to derived class
	\li 5. afterInitHook() which changes Lx,Ly,Lz if set by user

Note driver programs call only setFromOptions() and initFromOptions() (for IceModel 
or derived class).

Note IceModel::setFromOptions() should be called at the end of derivedClass:setFromOptions().

Note 2.5, 3, and 4 are called from initFromFile() in IceModel.

Note 3 and 4 are called from initFromOptions() in some derived classes (e.g. IceCompModel) 
in cases where initFromFile() is not called.

Note step 2.5 is skipped when bootstrapping (-bif and bootstrapFromFile_netCDF()) or in
those derived classes which can start with no input files, e.g. IceCompModel and IceEISModel.
That is, 2.5 is only done when starting from a saved model state.
*/
PetscErrorCode IceModel::createVecs() {
  PetscErrorCode ierr;

  if (createVecs_done == PETSC_TRUE) {
    ierr = destroyVecs(); CHKERRQ(ierr);
  }
  
  ierr = DACreateLocalVector(grid.da3, &vu); CHKERRQ(ierr);
  ierr = VecDuplicate(vu, &vv); CHKERRQ(ierr);
  ierr = VecDuplicate(vu, &vw); CHKERRQ(ierr);
  ierr = VecDuplicate(vu, &vSigma); CHKERRQ(ierr);
  ierr = VecDuplicate(vu, &vT); CHKERRQ(ierr);
  ierr = VecDuplicate(vu, &vtau); CHKERRQ(ierr);
  ierr = VecDuplicate(vu, &vgs); CHKERRQ(ierr);

  ierr = DACreateLocalVector(grid.da3b, &vTb); CHKERRQ(ierr);

  ierr = DACreateLocalVector(grid.da2, &vh); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vH); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vbed); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vAccum); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vTs); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vMask); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vGhf); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vubar); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vvbar); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vub); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vvb); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vRb); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vHmelt); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vbasalMeltRate); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vuplift); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vdHdt); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vbeta); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vtauc); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vtillphi); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vLongitude); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vLatitude); CHKERRQ(ierr);

  ierr = VecDuplicateVecs(vh, 2, &vDf); CHKERRQ(ierr);
  ierr = VecDuplicateVecs(vh, 2, &vuvbar); CHKERRQ(ierr);

  ierr = VecDuplicateVecs(vh, nWork2d, &vWork2d); CHKERRQ(ierr);
  ierr = VecDuplicateVecs(vu, nWork3d, &vWork3d); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vubarSAVE); CHKERRQ(ierr);
  ierr = VecDuplicate(vh, &vvbarSAVE); CHKERRQ(ierr);

  ierr = DACreateGlobalVector(grid.da2, &g2); CHKERRQ(ierr);
  ierr = DACreateGlobalVector(grid.da3, &g3); CHKERRQ(ierr);
  ierr = DACreateGlobalVector(grid.da3b, &g3b); CHKERRQ(ierr);

  const PetscInt M = 2 * grid.p->Mx * grid.p->My;
  ierr = MatCreateMPIAIJ(grid.com, PETSC_DECIDE, PETSC_DECIDE, M, M,
                         13, PETSC_NULL, 13, PETSC_NULL,
                         &SSAStiffnessMatrix); CHKERRQ(ierr);
  ierr = VecCreateMPI(grid.com, PETSC_DECIDE, M, &SSAX); CHKERRQ(ierr);
  ierr = VecDuplicate(SSAX, &SSARHS); CHKERRQ(ierr);
  ierr = VecCreateSeq(PETSC_COMM_SELF, M, &SSAXLocal);
  ierr = VecScatterCreate(SSAX, PETSC_NULL, SSAXLocal, PETSC_NULL,
                          &SSAScatterGlobalToLocal); CHKERRQ(ierr);
  ierr = KSPCreate(grid.com, &SSAKSP); CHKERRQ(ierr);

  createVecs_done = PETSC_TRUE;
  return 0;
}


//! De-allocate all Vecs defined in IceModel.
/*! 
Undoes the actions of createVecs().
 */
PetscErrorCode IceModel::destroyVecs() {
  PetscErrorCode ierr;

  ierr = bedDefCleanup(); CHKERRQ(ierr);
  ierr = PDDCleanup(); CHKERRQ(ierr);

  ierr = VecDestroy(vu); CHKERRQ(ierr);
  ierr = VecDestroy(vv); CHKERRQ(ierr);
  ierr = VecDestroy(vw); CHKERRQ(ierr);
  ierr = VecDestroy(vSigma); CHKERRQ(ierr);
  ierr = VecDestroy(vT); CHKERRQ(ierr);
  ierr = VecDestroy(vtau); CHKERRQ(ierr);
  ierr = VecDestroy(vgs); CHKERRQ(ierr);

  ierr = VecDestroy(vh); CHKERRQ(ierr);
  ierr = VecDestroy(vH); CHKERRQ(ierr);
  ierr = VecDestroy(vbed); CHKERRQ(ierr);
  ierr = VecDestroy(vAccum); CHKERRQ(ierr);
  ierr = VecDestroy(vTs); CHKERRQ(ierr);
  ierr = VecDestroy(vMask); CHKERRQ(ierr);
  ierr = VecDestroy(vGhf); CHKERRQ(ierr);
  ierr = VecDestroy(vubar); CHKERRQ(ierr);
  ierr = VecDestroy(vvbar); CHKERRQ(ierr);
  ierr = VecDestroy(vub); CHKERRQ(ierr);
  ierr = VecDestroy(vvb); CHKERRQ(ierr);
  ierr = VecDestroy(vRb); CHKERRQ(ierr);
  ierr = VecDestroy(vHmelt); CHKERRQ(ierr);
  ierr = VecDestroy(vbasalMeltRate); CHKERRQ(ierr);
  ierr = VecDestroy(vuplift); CHKERRQ(ierr);
  ierr = VecDestroy(vdHdt); CHKERRQ(ierr);
  ierr = VecDestroy(vbeta); CHKERRQ(ierr);
  ierr = VecDestroy(vtauc); CHKERRQ(ierr);
  ierr = VecDestroy(vtillphi); CHKERRQ(ierr);
  ierr = VecDestroy(vLongitude); CHKERRQ(ierr);
  ierr = VecDestroy(vLatitude); CHKERRQ(ierr);

  ierr = VecDestroyVecs(vuvbar, 2); CHKERRQ(ierr);
  ierr = VecDestroyVecs(vDf, 2); CHKERRQ(ierr);
  ierr = VecDestroyVecs(vWork3d, nWork3d); CHKERRQ(ierr);
  ierr = VecDestroyVecs(vWork2d, nWork2d); CHKERRQ(ierr);
  ierr = VecDestroy(vubarSAVE); CHKERRQ(ierr);
  ierr = VecDestroy(vvbarSAVE); CHKERRQ(ierr);

  ierr = VecDestroy(g2); CHKERRQ(ierr);
  ierr = VecDestroy(g3); CHKERRQ(ierr);
  ierr = VecDestroy(g3b); CHKERRQ(ierr);

  ierr = KSPDestroy(SSAKSP); CHKERRQ(ierr);
  ierr = MatDestroy(SSAStiffnessMatrix); CHKERRQ(ierr);
  ierr = VecDestroy(SSAX); CHKERRQ(ierr);
  ierr = VecDestroy(SSARHS); CHKERRQ(ierr);
  ierr = VecDestroy(SSAXLocal); CHKERRQ(ierr);
  ierr = VecScatterDestroy(SSAScatterGlobalToLocal); CHKERRQ(ierr);

  return 0;
}


void IceModel::setTimeStepYears(PetscScalar y) {
  dt = y * secpera;
  doAdaptTimeStep = PETSC_FALSE;
}

void IceModel::setMaxTimeStepYears(PetscScalar y) {
  maxdt = y * secpera;
  doAdaptTimeStep = PETSC_TRUE;
}

void IceModel::setAdaptTimeStepRatio(PetscScalar c) {
  adaptTimeStepRatio = c;
}

PetscErrorCode IceModel::setStartYear(PetscScalar y0) {
  startYear = y0;
  return 0;
}

PetscErrorCode IceModel::setEndYear(PetscScalar ye) {
    
  if (ye < startYear)   {
    SETERRQ(1, "ERROR: ye < startYear.  PISM cannot run backward in time.\n");
  }
  endYear = ye;
  return 0;
}

void  IceModel::setInitialAgeYears(PetscScalar d) {
  VecSet(vtau, d*secpera);
}

void IceModel::setAllGMaxVelocities(PetscScalar uvw_for_cfl) {
  gmaxu=uvw_for_cfl;
  gmaxv=uvw_for_cfl;
  gmaxw=uvw_for_cfl;
}

void IceModel::setConstantNuForSSA(PetscScalar nu) {
  useConstantNuForSSA = PETSC_TRUE;
  constantNuForSSA = nu;
}


PetscTruth IceModel::isInitialized() const {
  return initialized_p;
}


//! Update the surface elevation and the flow-type mask when the geometry has changed.
/*!
This procedure should be called whenever necessary to maintain consistency of geometry.
For instance, it should be called when either ice thickness or bed elevation change. 
In particular we always want \f$h = H + b\f$ to apply at grounded points, and, on the
other hand, we want the mask to reflect that the ice is floating if the floatation 
criterion applies at a point.

There is one difficult case.  When a point was floating and becomes grounded we generally
do not know whether to mark it as \c MASK_SHEET so that the SIA applies or \c MASK_DRAGGING
so that the SSA applies.  For now there is a vote-by-neighbors scheme (among the grounded 
neighbors).  When the \c MASK_DRAGGING points have plastic till bases this is not an issue.
 */
PetscErrorCode IceModel::updateSurfaceElevationAndMask() {
  PetscErrorCode ierr;
  PetscScalar **h, **bed, **H, **mask, ***T;
  const int MASK_GROUNDED_TO_DETERMINE = 999;

  ierr = DAVecGetArray(grid.da2, vh, &h); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vH, &H); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vbed, &bed); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vMask, &mask); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da3, vT, &T); CHKERRQ(ierr);

  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      // take this opportunity to check that H[i][j] >= 0
      if (H[i][j] < 0) {
        SETERRQ2(1,"Thickness negative at point i=%d, j=%d",i,j);
      }

      const PetscScalar hgrounded = bed[i][j] + H[i][j];

      if (isDrySimulation == PETSC_TRUE) {
        h[i][j] = hgrounded;
        // don't update mask; potentially one would want to do SSA
        //   dragging ice shelf in dry case and/or ignor mean sea level elevation
      } else {

        const PetscScalar hfloating = (1-ice.rho/ocean.rho) * H[i][j];
        if (modMask(mask[i][j]) == MASK_FLOATING) {
          // check whether you are actually floating or grounded
          if (hgrounded > hfloating+1.0) {
            mask[i][j] = MASK_GROUNDED_TO_DETERMINE;
            h[i][j] = hgrounded; // actually grounded so update h
          } else {
            h[i][j] = hfloating; // actually floating so update h
          }
        } else { // deal with grounded ice according to mask
          if (hgrounded > hfloating-1.0) {
            h[i][j] = hgrounded; // actually grounded so update h
          } else {
            mask[i][j] = MASK_FLOATING;
            h[i][j] = hfloating; // actually floating so update h
          }
        }

        if (intMask(mask[i][j]) == MASK_GROUNDED_TO_DETERMINE) {
          if (useSSAVelocity != PETSC_TRUE) {
            mask[i][j] = MASK_SHEET;
          } else {
            // if frozen to bed or essentially frozen to bed then make it SHEET
            if (T[i][j][0] + ice.beta_CC_grad * H[i][j] 
                         < DEFAULT_MIN_TEMP_FOR_SLIDING) { 
              mask[i][j] = MASK_SHEET;
            } else {
              // determine type of grounded ice by vote-by-neighbors
              //   (BOX stencil neighbors!):
              const PetscScalar neighmasksum = 
                modMask(mask[i-1][j+1]) + modMask(mask[i][j+1]) + modMask(mask[i+1][j+1]) +
                modMask(mask[i-1][j])   +                       + modMask(mask[i+1][j])  +
                modMask(mask[i-1][j-1]) + modMask(mask[i][j-1]) + modMask(mask[i+1][j-1]);
              // make SHEET if either all neighbors are SHEET or at most one is 
              //   DRAGGING; if any are floating then ends up DRAGGING:
              if (neighmasksum <= (7*MASK_SHEET + MASK_DRAGGING + 0.1)) { 
                mask[i][j] = MASK_SHEET;
              } else { // otherwise make DRAGGING
                mask[i][j] = MASK_DRAGGING;
              }
            }
          }
        }
        
      }

    }
  }

  ierr = DAVecRestoreArray(grid.da3, vT, &T); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vh, &h); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vH, &H); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vbed, &bed); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vMask, &mask); CHKERRQ(ierr);

  ierr = DALocalToLocalBegin(grid.da2, vh, INSERT_VALUES, vh); CHKERRQ(ierr);
  ierr = DALocalToLocalEnd(grid.da2, vh, INSERT_VALUES, vh); CHKERRQ(ierr);
  ierr = DALocalToLocalBegin(grid.da2, vMask, INSERT_VALUES, vMask); CHKERRQ(ierr);
  ierr = DALocalToLocalEnd(grid.da2, vMask, INSERT_VALUES, vMask); CHKERRQ(ierr);
  return 0;
}


/// Update the thickness from the velocity field and the surface and basal mass balance.
/*! 
@cond CONTINUUM
The partial differential equation describing the conservation of mass in the map-plane
(parallel to the geoid) is
  \f[ \frac{\partial H}{\partial t} = M - S - \nabla\cdot \mathbf{q} \f]
where 
  \f[ \mathbf{q} = \bar{\mathbf{U}} H.\f]
In these equations \f$H\f$ is the ice thickness, 
\f$M\f$ is the surface mass balance (accumulation or ablation), \f$S\f$ is the basal 
mass balance (e.g. basal melt), and \f$\bar{\mathbf{U}}\f$ is the vertically-averaged
horizontal velocity of the ice.  Note \f$\bar{\mathbf{U}} = (\bar u, \bar v)\f$ in the 
notation used here.  The map-plane flux of the ice \f$\mathbf{q}\f$ is defined by the 
above formula.

This procedure massBalExplicitStep() uses this conservation of mass equation to update 
the ice thickness.
@endcond

@cond NUMERIC
This method is first-order explicit in time.  The derivatives in \f$\nabla \cdot \mathbf{q}\f$ 
are computed by centered finite difference 
methods.  In the case of the SIA the value of \f$\bar{\mathbf{U}}\f$ is already stored on the 
staggered grid by velocitySIAStaggered() and it is differenced in the obvious centered manner
(with averaging of the thickness onto the staggered grid).

In the case of SSA locations the \f$\nabla \cdot \mathbf{q}\f$
term is computed by upwinding after expanding
  \f[ \nabla\cdot \mathbf{q} = \bar{\mathbf{U}} \cdot \nabla H + (\nabla \cdot \bar{\mathbf{U}}) H.\f]
That is, the mass conservation equation is regarded as an advection equation
with source term,
  \f[ \frac{\partial H}{\partial t} + \bar{\mathbf{U}} \cdot \nabla H 
                             = M - S - (\nabla \cdot \bar{\mathbf{U}}) H.\f]
The product of velocity and the gradient of thickness on the left is computed by first-order
upwinding.  Note that the CFL condition for this advection scheme is checked; see 
determineTimeStep().
@endcond

Note that if the point is flagged as \c MASK_FLOATING_OCEAN0 then the thickness is set to
zero.  Note that the rate of thickness change \f$\partial H/\partial t\f$ is computed and saved,
as is the rate of volume loss or gain.  Note updateSurfaceElevationAndMask() is called at the end.
 */
PetscErrorCode IceModel::massBalExplicitStep() {
  const PetscScalar   dx = grid.p->dx, dy = grid.p->dy;
  PetscErrorCode ierr;
  PetscScalar **H, **Hnew, **uvbar[2];
  PetscScalar **u, **v, **accum, **basalMeltRate, **mask;
  Vec vHnew = vWork2d[0];

  ierr = DAVecGetArray(grid.da2, vH, &H); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vbasalMeltRate, &basalMeltRate); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vAccum, &accum); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vMask, &mask); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vuvbar[0], &uvbar[0]); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vuvbar[1], &uvbar[1]); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vubar, &u); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vvbar, &v); CHKERRQ(ierr);
  ierr = VecCopy(vH, vHnew); CHKERRQ(ierr);
  ierr = DAVecGetArray(grid.da2, vHnew, &Hnew); CHKERRQ(ierr);

//ierr = verbPrintf(1,grid.com,
//       "entering massBalExplicitStep() with doOceanKill = %d and mask[0,0] = %d\n",
 //      doOceanKill,intMask(mask[0][0])); CHKERRQ(ierr);

  PetscScalar icecount = 0.0;
  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      if (H[i][j] > 0.0)  icecount++;
      PetscScalar divQ;
      if (intMask(mask[i][j]) == MASK_SHEET) { 
        // staggered grid Div(Q) for Q = D grad h
        const PetscScalar He = 0.5*(H[i][j] + H[i+1][j]);
        const PetscScalar Hw = 0.5*(H[i-1][j] + H[i][j]);
        const PetscScalar Hn = 0.5*(H[i][j] + H[i][j+1]);
        const PetscScalar Hs = 0.5*(H[i][j-1] + H[i][j]);
        divQ =  (uvbar[0][i][j] * He - uvbar[0][i-1][j] * Hw) / dx
              + (uvbar[1][i][j] * Hn - uvbar[1][i][j-1] * Hs) / dy;
      } else { 
        divQ =
          u[i][j] * (u[i][j] < 0 ? H[i+1][j]-H[i][j] : H[i][j]-H[i-1][j]) / dx
          + v[i][j] * (v[i][j] < 0 ? H[i][j+1]-H[i][j] : H[i][j]-H[i][j-1]) / dy
          + H[i][j] * ((u[i+1][j]-u[i-1][j])/(2*dx) + (v[i][j+1]-v[i][j-1])/(2*dy));
      }

      Hnew[i][j] += (accum[i][j] - divQ) * dt;
      if (includeBMRinContinuity == PETSC_TRUE) {
         Hnew[i][j] -= capBasalMeltRate(basalMeltRate[i][j]) * dt;
      }

      // apply free boundary rule: negative thickness becomes zero
      if (Hnew[i][j] < 0)
        Hnew[i][j] = 0.0;
      // force zero at ocean; "accumulation-zone" b.c.
      if ( (doOceanKill == PETSC_TRUE) && (intMask(mask[i][j]) == MASK_FLOATING_OCEAN0) )
        Hnew[i][j] = 0.0;
    }
  }

  ierr = DAVecRestoreArray(grid.da2, vbasalMeltRate, &basalMeltRate); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vAccum, &accum); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vMask, &mask); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vuvbar[0], &uvbar[0]); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vuvbar[1], &uvbar[1]); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vubar, &u); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vvbar, &v); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vH, &H); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(grid.da2, vHnew, &Hnew); CHKERRQ(ierr);

  // compute dH/dt (thickening rate) for viewing and for saving at end; only diagnostic
  ierr = VecWAXPY(vdHdt, -1, vH, vHnew); CHKERRQ(ierr);
  ierr = VecScale(vdHdt,1.0/dt); CHKERRQ(ierr);

  // average value of dH/dt; also d(volume)/dt
  PetscScalar gicecount;
  ierr = PetscGlobalSum(&icecount, &gicecount, grid.com); CHKERRQ(ierr);
  ierr = DALocalToGlobal(grid.da2, vdHdt, INSERT_VALUES, g2); CHKERRQ(ierr);
  ierr = VecSum(g2, &gdHdtav); CHKERRQ(ierr);
  dvoldt = gdHdtav * grid.p->dx * grid.p->dy;  // m^3/s
  gdHdtav = gdHdtav / gicecount; // m/s

  // finally copy vHnew into vH (and communicate ghosted values at same time)
  ierr = DALocalToLocalBegin(grid.da2, vHnew, INSERT_VALUES, vH); CHKERRQ(ierr);
  ierr = DALocalToLocalEnd(grid.da2, vHnew, INSERT_VALUES, vH); CHKERRQ(ierr);

  // update h and mask
  ierr = updateSurfaceElevationAndMask(); CHKERRQ(ierr);
  return 0;
}


/// Do the time-stepping for an evolution run.
/*! 
This important routine can be replaced by derived classes; it is \c virtual.

This procedure has the main loop.  The following actions are taken on each pass through the loop:
\li the yield stress for the plastic till model is updated (if appropriate)
\li the positive degree day model is invoked to compute the surface mass balance (if appropriate)
\li a step of the bed deformation model is taken (if appropriate)
\li the velocity field is updated; in some cases the whole three-dimensional field is updated 
    and in some cases just the vertically-averaged horizontal velocity is updated; see velocity()
\li the time step is determined according to a variety of stability criteria; 
    see determineTimeStep()
\li the temperature field is updated according to the conservation of energy model based 
    (especially) on the new velocity field; see temperatureAgeStep()
\li the thickness of the ice is updated according to the mass conservation model; see
    massBalExplicitStep()
\li there is various reporting to the user on the current state; see summary() and updateViewers()

Note that at the beginning and ends of the loop there is a chance for derived classes to do extra work.  See additionalAtStartTimestep() and additionalAtEndTimestep().
 */
PetscErrorCode IceModel::run() {
  PetscErrorCode  ierr;

  ierr = verbPrintf(2,grid.com, "     "); CHKERRQ(ierr);
  ierr = summaryPrintLine(PETSC_TRUE,doTemp, 0.0, 0.0, 0, ' ', 0.0, 0.0, 0.0, 0.0, 0.0); CHKERRQ(ierr);
  ierr = verbPrintf(2,grid.com, "$$$$$"); CHKERRQ(ierr);
  adaptReasonFlag = ' '; // no reason for no timestep
  tempskipCountDown = 0;
  ierr = summary(doTemp,reportHomolTemps); CHKERRQ(ierr);  // report starting state

  dtTempAge = 0.0;
  // main loop for time evolution
  for (PetscScalar year = startYear; year < endYear; year += dt/secpera) {
    ierr = verbPrintf(2,grid.com, "$"); CHKERRQ(ierr);
    dt_force = -1.0;
    maxdt_temporary = -1.0;
    ierr = additionalAtStartTimestep(); CHKERRQ(ierr);  // might set dt_force,maxdt_temporary
    
    // update basal till yield stress if appropriate; will modify and communicate mask
    if (doPlasticTill == PETSC_TRUE) {
      ierr = updateYieldStressFromHmelt();  CHKERRQ(ierr);
    }
    
    // compute PDD; generates net accumulation, with possible ablation area, using snow accumulation
    // might set maxdt_temporary 
    if ((doPDD == PETSC_TRUE) && IsIntegralYearPDD()) {
      ierr = updateNetAccumFromPDD();  CHKERRQ(ierr);
    }

    // compute bed deformation, which only depends on current thickness and bed elevation
    if (doBedDef == PETSC_TRUE) {
      ierr = bedDefStepIfNeeded(); CHKERRQ(ierr);
    } else {
      ierr = verbPrintf(2,grid.com, "$"); CHKERRQ(ierr);
    }

    // always do vertically-average velocity calculation; only update velocities at depth if
    // needed for temp and age calculation
    bool updateAtDepth = (tempskipCountDown == 0);
    ierr = velocity(updateAtDepth); CHKERRQ(ierr);
    ierr = verbPrintf(2,grid.com, updateAtDepth ? "v" : "V" ); CHKERRQ(ierr);

    // now that velocity field is up to date, compute grain size
    if (doGrainSize == PETSC_TRUE) {
      ierr = updateGrainSizeIfNeeded(); CHKERRQ(ierr);
    }
    
    // adapt time step using velocities and diffusivity, ..., just computed
    bool useCFLforTempAgeEqntoGetTimestep = (doTemp == PETSC_TRUE);
    ierr = determineTimeStep(useCFLforTempAgeEqntoGetTimestep); CHKERRQ(ierr);
    dtTempAge += dt;
    grid.p->year += dt / secpera;  // adopt it
    // IceModel::dt,dtTempAge,grid.p->year are now set correctly according to
    //    mass-continuity-eqn-diffusivity criteria, CFL criteria, and other 
    //    criteria from derived class additionalAtStartTimestep(), and from 
    //    "-tempskip" mechanism

    // ierr = PetscPrintf(PETSC_COMM_SELF,
    //           "\n[rank=%d, year=%f, dt=%f, startYear=%f, endYear=%f]",
    //           grid.rank, grid.p->year, dt/secpera, startYear, endYear);
    //        CHKERRQ(ierr);
    
    bool tempAgeStep = (updateAtDepth && (doTemp == PETSC_TRUE));
    if (tempAgeStep) { // do temperature and age
      ierr = temperatureAgeStep(); CHKERRQ(ierr);
      dtTempAge = 0.0;
      ierr = verbPrintf(2,grid.com, "t"); CHKERRQ(ierr);
    } else {
      ierr = verbPrintf(2,grid.com, "$"); CHKERRQ(ierr);
    }
    
    if (doMassConserve == PETSC_TRUE) {
      ierr = massBalExplicitStep(); CHKERRQ(ierr);
      if ((doTempSkip == PETSC_TRUE) && (tempskipCountDown > 0))
        tempskipCountDown--;
      ierr = verbPrintf(2,grid.com, "f"); CHKERRQ(ierr);
    } else {
      ierr = verbPrintf(2,grid.com, "$"); CHKERRQ(ierr);
    }
    
    ierr = additionalAtEndTimestep(); CHKERRQ(ierr);

    ierr = summary(tempAgeStep,reportHomolTemps); CHKERRQ(ierr);

    ierr = updateViewers(); CHKERRQ(ierr);

    if (endOfTimeStepHook() != 0) break;
  }
  
  return 0;
}


/// Calls the necessary routines to do a diagnostic calculation of velocity.
/*! 
This important routine can be replaced by derived classes; it is \c virtual.

This procedure has no loop but the following actions are taken:
\li the yield stress for the plastic till model is updated (if appropriate)
\li the velocity field is updated; in some cases the whole three-dimensional field is updated 
    and in some cases just the vertically-averaged horizontal velocity is updated; see velocity()
\li there is various reporting to the user on the current state; see summary() and updateViewers()
 */
PetscErrorCode IceModel::diagnosticRun() {
  PetscErrorCode  ierr;

  // print out some stats about input state
  ierr = verbPrintf(2,grid.com, "     "); CHKERRQ(ierr);
  ierr = summaryPrintLine(PETSC_TRUE,PETSC_TRUE, 0.0, 0.0, 0, ' ', 0.0, 0.0, 0.0, 0.0, 0.0);
           CHKERRQ(ierr);
  ierr = verbPrintf(2,grid.com, "$$$$$"); CHKERRQ(ierr);
  adaptReasonFlag = ' '; // no reason for no timestep
  tempskipCountDown = 0;

  // update basal till yield stress if appropriate; will modify and communicate mask
  if (doPlasticTill == PETSC_TRUE) {
    ierr = updateYieldStressFromHmelt();  CHKERRQ(ierr);
  }

  ierr = velocity(true); CHKERRQ(ierr);  // compute velocities (at depth)

  ierr = summary(true,true); CHKERRQ(ierr);
  
  // update viewers and pause for a chance to view
  ierr = updateViewers(); CHKERRQ(ierr);
  PetscInt    pause_time = 0;
  ierr = PetscOptionsGetInt(PETSC_NULL, "-pause", &pause_time, PETSC_NULL); CHKERRQ(ierr);
  if (pause_time > 0) {
    ierr = verbPrintf(2,grid.com,"pausing for %d secs ...\n",pause_time); CHKERRQ(ierr);
    ierr = PetscSleep(pause_time); CHKERRQ(ierr);
  }
  return 0;
}


// note no range checking in these two:
int IceModel::intMask(PetscScalar maskvalue) {
  return static_cast<int>(floor(maskvalue + 0.5));
}

int IceModel::modMask(PetscScalar maskvalue) {
  int intmask = static_cast<int>(floor(maskvalue + 0.5));
  if (intmask > MASK_FLOATING) {
    return intmask - 4;
  } else {
    return intmask;
  }
}
