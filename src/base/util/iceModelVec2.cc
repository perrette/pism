// Copyright (C) 2008--2011 Ed Bueler and Constantine Khroulev
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

#include <cstring>
#include <cstdlib>
#include <petscda.h>
#include "NCTool.hh"

#include "iceModelVec.hh"

// this file contains methods for derived classes IceModelVec2S and IceModelVec2Int

// methods for base class IceModelVec are in "iceModelVec.cc"

PetscErrorCode  IceModelVec2S::create(IceGrid &my_grid, string my_name, bool local, int width) {
  if (!utIsInit()) {
    SETERRQ(1, "PISM ERROR: UDUNITS *was not* initialized.\n");
  }

  if (v != PETSC_NULL) {
    SETERRQ1(2,"IceModelVec2S with name='%s' already allocated\n", my_name.c_str());
  }
  PetscErrorCode ierr = IceModelVec2::create(my_grid, my_name, local, width, dof); CHKERRQ(ierr);
  return 0;
}

PetscErrorCode IceModelVec2S::get_array(PetscScalar** &a) {
  PetscErrorCode ierr;
  ierr = begin_access(); CHKERRQ(ierr);
  a = static_cast<PetscScalar**>(array);
  return 0;
}

//! Puts a local IceModelVec2S on processor 0.
/*!
 <ul>
 <li> onp0 and ctx should be created by calling VecScatterCreateToZero or be identical to one,
 <li> g2 is a preallocated temporary global vector,
 <li> g2natural is a preallocated temporary global vector with natural ordering.
 </ul>
*/
PetscErrorCode IceModelVec2S::put_on_proc0(Vec onp0, VecScatter ctx, Vec g2, Vec g2natural) {
  PetscErrorCode ierr;
  ierr = checkAllocated(); CHKERRQ(ierr);

  if (!localp)
    SETERRQ1(1, "Can't put a global IceModelVec '%s' on proc 0.", name.c_str());

  ierr =        DALocalToGlobal(da, v,  INSERT_VALUES, g2);        CHKERRQ(ierr);
  ierr = DAGlobalToNaturalBegin(grid->da2, g2, INSERT_VALUES, g2natural); CHKERRQ(ierr);
  ierr =   DAGlobalToNaturalEnd(grid->da2, g2, INSERT_VALUES, g2natural); CHKERRQ(ierr);

  ierr = VecScatterBegin(ctx, g2natural, onp0, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);
  ierr =   VecScatterEnd(ctx, g2natural, onp0, INSERT_VALUES, SCATTER_FORWARD); CHKERRQ(ierr);

  return 0;
}

//! Gets a local IceModelVec2 from processor 0.
/*!
 <ul>
 <li> onp0 and ctx should be created by calling VecScatterCreateToZero or be identical to one,
 <li> g2 is a preallocated temporary global vector,
 <li> g2natural is a preallocated temporary global vector with natural ordering.
 </ul>
*/
PetscErrorCode IceModelVec2S::get_from_proc0(Vec onp0, VecScatter ctx, Vec g2, Vec g2natural) {
  PetscErrorCode ierr;
  ierr = checkAllocated(); CHKERRQ(ierr);

  if (!localp)
    SETERRQ1(1, "Can't get a global IceModelVec '%s' from proc 0.", name.c_str());

  ierr = VecScatterBegin(ctx, onp0, g2natural, INSERT_VALUES, SCATTER_REVERSE); CHKERRQ(ierr);
  ierr =   VecScatterEnd(ctx, onp0, g2natural, INSERT_VALUES, SCATTER_REVERSE); CHKERRQ(ierr);

  ierr = DANaturalToGlobalBegin(grid->da2, g2natural, INSERT_VALUES, g2); CHKERRQ(ierr);
  ierr =   DANaturalToGlobalEnd(grid->da2, g2natural, INSERT_VALUES, g2); CHKERRQ(ierr);
  ierr =   DAGlobalToLocalBegin(da, g2,               INSERT_VALUES, v);  CHKERRQ(ierr);
  ierr =     DAGlobalToLocalEnd(da, g2,               INSERT_VALUES, v);  CHKERRQ(ierr);

  return 0;
}

//! Sets an IceModelVec2 to the magnitude of a 2D vector field with components \c v_x and \c v_y.
/*! Computes the magnitude \b pointwise, so any of v_x, v_y and the IceModelVec
  this is called on can be the same.

  Does not communicate.
 */
PetscErrorCode IceModelVec2S::set_to_magnitude(IceModelVec2S &v_x, IceModelVec2S &v_y) {
  PetscErrorCode ierr;
  PetscScalar **mag = NULL;
  ierr = v_x.begin_access(); CHKERRQ(ierr);
  ierr = v_y.begin_access(); CHKERRQ(ierr);
  ierr = get_array(mag); CHKERRQ(ierr);
  for (PetscInt i=grid->xs; i<grid->xs+grid->xm; ++i) {
    for (PetscInt j=grid->ys; j<grid->ys+grid->ym; ++j) {
      mag[i][j] = sqrt( PetscSqr(v_x(i,j)) + PetscSqr(v_y(i,j)) );
    }
  }
  ierr = v_x.end_access(); CHKERRQ(ierr);
  ierr = v_y.end_access(); CHKERRQ(ierr);
  ierr = end_access(); CHKERRQ(ierr);
  
  return 0;
}

//! Masks out all the areas where \f$ M \le 0 \f$ by setting them to \c fill. 
PetscErrorCode IceModelVec2S::mask_by(IceModelVec2S &M, PetscScalar fill) {
  PetscErrorCode ierr;
  PetscScalar **a = NULL;
  ierr = get_array(a); CHKERRQ(ierr);
  ierr = M.begin_access(); CHKERRQ(ierr);
  for (PetscInt i=grid->xs; i<grid->xs+grid->xm; ++i) {
    for (PetscInt j=grid->ys; j<grid->ys+grid->ym; ++j) {
      if (M(i,j) <= 0.0)
	a[i][j] = fill;
    }
  }
  ierr = end_access(); CHKERRQ(ierr);
  ierr = M.end_access(); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode IceModelVec2::write(string filename, nc_type nctype) {
  PetscErrorCode ierr;

  ierr = checkAllocated(); CHKERRQ(ierr);

  Vec tmp;			// a temporary one-component vector,
				// distributed across processors the same way v is

  // The simplest case:
  if ((dof == 1) && (localp == false)) {
    ierr = IceModelVec::write(filename, nctype); CHKERRQ(ierr);
    return 0;
  }

  ierr = DACreateGlobalVector(grid->da2, &tmp); CHKERRQ(ierr);

  for (int j = 0; j < dof; ++j) {
    vars[j].time_independent = time_independent;
    ierr = IceModelVec2::get_component(j, tmp); CHKERRQ(ierr);
    ierr = vars[j].write(filename, nctype,
			 write_in_glaciological_units, tmp);
  }

  // Clean up:
  ierr = VecDestroy(tmp);
  return 0;
}

PetscErrorCode IceModelVec2::read(string filename, const unsigned int time) {
  PetscErrorCode ierr;

  if ((dof == 1) && (localp == false)) {
    ierr = IceModelVec::read(filename, time); CHKERRQ(ierr);
    return 0;
  }

  ierr = checkAllocated(); CHKERRQ(ierr);

  Vec tmp;			// a temporary one-component vector,
				// distributed across processors the same way v is
  ierr = DACreateGlobalVector(grid->da2, &tmp); CHKERRQ(ierr);

  for (int j = 0; j < dof; ++j) {
    ierr = vars[j].read(filename, time, tmp); CHKERRQ(ierr);
    ierr = IceModelVec2::set_component(j, tmp); CHKERRQ(ierr);
  }
  
  // The calls above only set the values owned by a processor, so we need to
  // communicate if localp == true:
  if (localp) {
    ierr = beginGhostComm(); CHKERRQ(ierr);
    ierr = endGhostComm(); CHKERRQ(ierr);
  }

  // Clean up:
  ierr = VecDestroy(tmp); CHKERRQ(ierr);
  return 0;  
}

PetscErrorCode IceModelVec2::regrid(string filename, bool critical, int start) {
  PetscErrorCode ierr;
  LocalInterpCtx *lic = NULL;

  if ((dof == 1) && (localp == false)) {
    ierr = IceModelVec::regrid(filename, critical, start); CHKERRQ(ierr);
    return 0;
  }
  
  ierr = get_interp_context(filename, lic); CHKERRQ(ierr);
  if (lic != NULL) {
    lic->start[0] = start;
    lic->report_range = report_range;
  }

  Vec tmp;			// a temporary one-component vector,
				// distributed across processors the same way v is
  ierr = DACreateGlobalVector(grid->da2, &tmp); CHKERRQ(ierr);

  for (int j = 0; j < dof; ++j) {
    ierr = vars[j].regrid(filename, lic, critical, false, 0.0, tmp); CHKERRQ(ierr);
    ierr = IceModelVec2::set_component(j, tmp); CHKERRQ(ierr);
  }

  // The calls above only set the values owned by a processor, so we need to
  // communicate if localp == true:
  if (localp) {
    ierr = beginGhostComm(); CHKERRQ(ierr);
    ierr = endGhostComm(); CHKERRQ(ierr);
  }

  // Clean up:
  ierr = VecDestroy(tmp);
  delete lic;
  return 0;
}

PetscErrorCode IceModelVec2::regrid(string filename, PetscScalar default_value) {
  PetscErrorCode ierr;
  LocalInterpCtx *lic = NULL;

  if ((dof == 1) && (localp == false)) {
    ierr = IceModelVec::regrid(filename, default_value); CHKERRQ(ierr);
    return 0;
  }
  
  ierr = get_interp_context(filename, lic); CHKERRQ(ierr);
  if (lic != NULL) {
    lic->report_range = report_range;
  }

  Vec tmp;			// a temporary one-component vector,
				// distributed across processors the same way v is
  ierr = DACreateGlobalVector(grid->da2, &tmp); CHKERRQ(ierr);

  for (int j = 0; j < dof; ++j) {
    ierr = vars[j].regrid(filename, lic, false, true, default_value, tmp); CHKERRQ(ierr);
    ierr = IceModelVec2::set_component(j, tmp); CHKERRQ(ierr);
  }

  // The calls above only set the values owned by a processor, so we need to
  // communicate if localp == true:
  if (localp) {
    ierr = beginGhostComm(); CHKERRQ(ierr);
    ierr = endGhostComm(); CHKERRQ(ierr);
  }

  // Clean up:
  ierr = VecDestroy(tmp);
  delete lic;
  return 0;
}

//! \brief View a 2D field.
PetscErrorCode IceModelVec2::view(PetscInt viewer_size) {
  PetscErrorCode ierr;
  PetscViewer viewers[2] = {PETSC_NULL, PETSC_NULL};

  if (dof > 2) SETERRQ(1, "dof > 2 is not supported");

  for (int j = 0; j < dof; ++j) {
    string c_name = vars[j].short_name,
      long_name = vars[j].get_string("long_name"),
      units = vars[j].get_string("glaciological_units"),
      title = long_name + " (" + units + ")";

    if ((*map_viewers)[c_name] == PETSC_NULL) {
      ierr = grid->create_viewer(viewer_size, title, (*map_viewers)[c_name]); CHKERRQ(ierr);
    }

    viewers[j] = (*map_viewers)[c_name];
  }

  ierr = view(viewers[0], viewers[1]); CHKERRQ(ierr); 

  return 0;
}

//! \brief View a 2D vector field using existing PETSc viewers.
//! Allocates and de-allocates g2, the temporary global vector; performance
//! should not matter here.
PetscErrorCode IceModelVec2::view(PetscViewer v1, PetscViewer v2) {
  PetscErrorCode ierr;
  Vec g2;

  ierr = DACreateGlobalVector(grid->da2, &g2); CHKERRQ(ierr);

  PetscViewer viewers[2] = {v1, v2};

  for (int i = 0; i < dof; ++i) {
    string long_name = vars[i].get_string("long_name"),
      units = vars[i].get_string("glaciological_units"),
      title = long_name + " (" + units + ")";

    PetscDraw draw;
    ierr = PetscViewerDrawGetDraw(viewers[i], 0, &draw); CHKERRQ(ierr);
    ierr = PetscDrawSetTitle(draw, title.c_str()); CHKERRQ(ierr);

    ierr = IceModelVec2::get_component(i, g2); CHKERRQ(ierr);

    ierr = vars[i].to_glaciological_units(g2); CHKERRQ(ierr);

    ierr = VecView(g2, viewers[i]); CHKERRQ(ierr);
  }

  ierr = VecDestroy(g2); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode IceModelVec2S::view_matlab(PetscViewer my_viewer) {
  PetscErrorCode ierr;
  string long_name = vars[0].get_string("long_name");
  Vec g2;

  ierr = DACreateGlobalVector(grid->da2, &g2); CHKERRQ(ierr);

  if (localp) {
    ierr = copy_to(g2); CHKERRQ(ierr);
  } else {
    ierr = VecCopy(v, g2); CHKERRQ(ierr);
  }

  ierr = vars[0].to_glaciological_units(g2); CHKERRQ(ierr);

  // add Matlab comment before listing, using short title

  ierr = PetscViewerASCIIPrintf(my_viewer, "\n%%%% %s = %s\n",
				name.c_str(), long_name.c_str()); CHKERRQ(ierr);
  ierr = PetscObjectSetName((PetscObject) g2, name.c_str()); CHKERRQ(ierr);

  ierr = VecView(g2, my_viewer); CHKERRQ(ierr);

  ierr = PetscViewerASCIIPrintf(my_viewer,"\n%s = reshape(%s,%d,%d);\n\n",
				name.c_str(), name.c_str(), grid->My, grid->Mx); CHKERRQ(ierr);

  ierr = VecDestroy(g2); CHKERRQ(ierr);

  return 0;
}

//! Checks if the current IceModelVec2S has NANs and reports if it does.
/*! Up to a fixed number of messages are printed at stdout.  Returns the full
 count of NANs (which is a nonzero) on this rank. */
PetscErrorCode IceModelVec2S::has_nan() {
  PetscErrorCode ierr;
  const PetscInt max_print_this_rank=10;
  PetscInt retval=0;

  ierr = begin_access(); CHKERRQ(ierr);
  PetscInt i, j;
  for (i=grid->xs; i<grid->xs+grid->xm; ++i) {
    for (j=grid->ys; j<grid->ys+grid->ym; ++j) {
      if (gsl_isnan((*this)(i,j))) {
        retval++;
        if (retval <= max_print_this_rank) {
          ierr = PetscSynchronizedPrintf(grid->com, 
             "IceModelVec2S %s: NAN (or uninitialized) at i = %d, j = %d on rank = %d\n",
             name.c_str(), i, j, grid->rank); CHKERRQ(ierr);
        }
      }
    }
  }
  ierr = end_access(); CHKERRQ(ierr);

  if (retval > 0) {
    ierr = PetscSynchronizedPrintf(grid->com, 
       "IceModelVec2S %s: detected %d NANs (or uninitialized) on rank = %d\n",
             name.c_str(), retval, grid->rank); CHKERRQ(ierr);
  }

  ierr = PetscSynchronizedFlush(grid->com); CHKERRQ(ierr);
  return retval;
}

//! \brief Returns the x-derivative at i,j approximated using centered finite
//! differences.
PetscScalar IceModelVec2S::diff_x(int i, int j) {
  return ( (*this)(i + 1,j) - (*this)(i - 1,j) ) / (2 * grid->dx);
}

//! \brief Returns the y-derivative at i,j approximated using centered finite
//! differences.
PetscScalar IceModelVec2S::diff_y(int i, int j) {
  return ( (*this)(i,j + 1) - (*this)(i,j - 1) ) / (2 * grid->dy);
}


//! \brief Returns the x-derivative at East staggered point i+1/2,j approximated 
//! using centered (obvious) finite differences.
PetscScalar IceModelVec2S::diff_x_stagE(int i, int j) {
  return ( (*this)(i+1,j) - (*this)(i,j) ) / (grid->dx);
}

//! \brief Returns the y-derivative at East staggered point i+1/2,j approximated 
//! using centered finite differences.
PetscScalar IceModelVec2S::diff_y_stagE(int i, int j) {
  return (   (*this)(i+1,j+1) + (*this)(i,j+1)
           - (*this)(i+1,j-1) - (*this)(i,j-1) ) / ( 4* grid->dy);
}

//! \brief Returns the x-derivative at North staggered point i,j+1/2 approximated 
//! using centered finite differences.
PetscScalar IceModelVec2S::diff_x_stagN(int i, int j) {
  return (   (*this)(i+1,j+1) + (*this)(i+1,j)
           - (*this)(i-1,j+1) - (*this)(i-1,j) ) / ( 4* grid->dx);
}

//! \brief Returns the y-derivative at North staggered point i,j+1/2 approximated 
//! using centered (obvious) finite differences.
PetscScalar IceModelVec2S::diff_y_stagN(int i, int j) {
  return ( (*this)(i,j+1) - (*this)(i,j) ) / (grid->dy);
}


//! \brief Returns the x-derivative at i,j approximated using centered finite
//! differences. Respects grid periodicity and uses one-sided FD at grid edges
//! if necessary.
PetscScalar IceModelVec2S::diff_x_p(int i, int j) {
  if (grid->periodicity & X_PERIODIC)
    return diff_x(i,j);
  
  if (i == 0)
    return ( (*this)(i + 1,j) - (*this)(i,j) ) / (grid->dx);
  else if (i == grid->Mx - 1)
    return ( (*this)(i,j) - (*this)(i - 1,j) ) / (grid->dx);
  else
    return diff_x(i,j);
}

//! \brief Returns the y-derivative at i,j approximated using centered finite
//! differences. Respects grid periodicity and uses one-sided FD at grid edges
//! if necessary.
PetscScalar IceModelVec2S::diff_y_p(int i, int j) {
  if (grid->periodicity & Y_PERIODIC)
    return diff_y(i,j);
  
  if (j == 0)
    return ( (*this)(i,j + 1) - (*this)(i,j) ) / (grid->dy);
  else if (j == grid->My - 1)
    return ( (*this)(i,j) - (*this)(i,j - 1) ) / (grid->dy);
  else
    return diff_y(i,j);
}

//! Sums up all the values in an IceModelVec2S object. Ignores ghosts.
/*! Avoids copying to a "global" vector.
 */
PetscErrorCode IceModelVec2S::sum(PetscScalar &result) {
  PetscErrorCode ierr;
  PetscScalar my_result = 0;

  // sum up the local part:
  ierr = begin_access(); CHKERRQ(ierr);
  for (PetscInt i=grid->xs; i<grid->xs+grid->xm; ++i) {
    for (PetscInt j=grid->ys; j<grid->ys+grid->ym; ++j) {
      my_result += (*this)(i,j);
    }
  }
  ierr = end_access(); CHKERRQ(ierr);

  // find the global sum:
  ierr = PetscGlobalSum(&my_result, &result, grid->com); CHKERRQ(ierr);

  return 0;
}


//! Finds maximum over all the values in an IceModelVec2S object.  Ignores ghosts.
PetscErrorCode IceModelVec2S::max(PetscScalar &result) {
  PetscErrorCode ierr;
  ierr = begin_access(); CHKERRQ(ierr);
  PetscScalar my_result = (*this)(grid->xs,grid->ys);
  for (PetscInt i=grid->xs; i<grid->xs+grid->xm; ++i) {
    for (PetscInt j=grid->ys; j<grid->ys+grid->ym; ++j) {
      my_result = PetscMax(my_result,(*this)(i,j));
    }
  }
  ierr = end_access(); CHKERRQ(ierr);
  ierr = PetscGlobalMax(&my_result, &result, grid->com); CHKERRQ(ierr);
  return 0;
}


//! Finds minimum over all the values in an IceModelVec2S object.  Ignores ghosts.
PetscErrorCode IceModelVec2S::min(PetscScalar &result) {
  PetscErrorCode ierr;
  ierr = begin_access(); CHKERRQ(ierr);
  PetscScalar my_result = (*this)(grid->xs,grid->ys);
  for (PetscInt i=grid->xs; i<grid->xs+grid->xm; ++i) {
    for (PetscInt j=grid->ys; j<grid->ys+grid->ym; ++j) {
      my_result = PetscMin(my_result,(*this)(i,j));
    }
  }
  ierr = end_access(); CHKERRQ(ierr);
  ierr = PetscGlobalMin(&my_result, &result, grid->com); CHKERRQ(ierr);
  return 0;
}


// IceModelVec2

/*!
 * This could be implemented using VecStrideGather, but our code is more
 * flexible: \c source and the current IceModelVec2 need not be both local or
 * global.
 */

PetscErrorCode IceModelVec2::get_component(int N, Vec result) {
  PetscErrorCode ierr;
  void *tmp_res = NULL, *tmp_v;

  if (N < 0 || N >= dof)
    SETERRQ(1, "invalid argument (N)");

  ierr = DAVecGetArray(grid->da2, result, &tmp_res); CHKERRQ(ierr);
  PetscScalar **res = static_cast<PetscScalar**>(tmp_res);

  ierr = DAVecGetArrayDOF(da, v, &tmp_v); CHKERRQ(ierr);
  PetscScalar ***a_dof = static_cast<PetscScalar***>(tmp_v);

  for (PetscInt i = grid->xs; i < grid->xs+grid->xm; ++i)
    for (PetscInt j = grid->ys; j < grid->ys+grid->ym; ++j)
      res[i][j] = a_dof[i][j][N];


  ierr = DAVecRestoreArray(grid->da2, result, &tmp_res); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(da,        v,      &tmp_v); CHKERRQ(ierr);

  return 0;
}

/*!
 * This could be implemented using VecStrideScatter, but our code is more
 * flexible: \c source and the current IceModelVec2 need not be both local or
 * global.
 */
PetscErrorCode IceModelVec2::set_component(int N, Vec source) {
  PetscErrorCode ierr;
  void *tmp_src = NULL, *tmp_v;

  if (N < 0 || N >= dof)
    SETERRQ(1, "invalid argument (N)");

  ierr = DAVecGetArray(grid->da2, source, &tmp_src); CHKERRQ(ierr);
  PetscScalar **src = static_cast<PetscScalar**>(tmp_src);

  ierr = DAVecGetArrayDOF(da, v, &tmp_v); CHKERRQ(ierr);
  PetscScalar ***a_dof = static_cast<PetscScalar***>(tmp_v);

  for (PetscInt i = grid->xs; i < grid->xs+grid->xm; ++i)
    for (PetscInt j = grid->ys; j < grid->ys+grid->ym; ++j)
      a_dof[i][j][N] = src[i][j];

  ierr = DAVecRestoreArray(grid->da2, source, &tmp_src); CHKERRQ(ierr);
  ierr = DAVecRestoreArray(da,        v,      &tmp_v); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode IceModelVec2::get_component(int n, IceModelVec2 &result) {
  PetscErrorCode ierr;

  ierr = IceModelVec2::get_component(n, result.v); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode IceModelVec2::set_component(int n, IceModelVec2 &source) {
  PetscErrorCode ierr;

  ierr = IceModelVec2::set_component(n, source.v); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode  IceModelVec2::create(IceGrid &my_grid, string my_name, bool local,
                                     int stencil_width, int my_dof) {
  PetscErrorCode ierr;

  if (!utIsInit()) {
    SETERRQ(1, "PISM ERROR: UDUNITS *was not* initialized.\n");
  }
  if (v != PETSC_NULL) {
    SETERRQ1(2,"IceModelVec2 with name='%s' already allocated\n", my_name.c_str());
  }

  dof  = my_dof;
  grid = &my_grid;

  if ((dof != 1) || (stencil_width > grid->max_stencil_width)) {
    da_stencil_width = stencil_width;
    ierr = create_2d_da(da, dof, da_stencil_width); CHKERRQ(ierr);
  } else {
    da_stencil_width = grid->max_stencil_width;
    da = grid->da2;
  }

  if (local) {
    ierr = DACreateLocalVector(da, &v); CHKERRQ(ierr);
  } else {
    ierr = DACreateGlobalVector(da, &v); CHKERRQ(ierr);
  }

  localp = local;
  name = my_name;

  vars.resize(dof);
  for (int j = 0; j < dof; ++j)
    vars[j].init_2d(my_name, my_grid);

  //  ierr = this->set(GSL_NAN); CHKERRQ(ierr);

  return 0;
}

// IceModelVec2Stag

PetscErrorCode  IceModelVec2Stag::create(IceGrid &my_grid, string my_short_name, bool local,
					 int stencil_width) {

  PetscErrorCode ierr = IceModelVec2::create(my_grid, my_short_name, local,
					     stencil_width, dof); CHKERRQ(ierr);
  string s_name = name;
  vars[0].init_2d(s_name + "[0]", my_grid);
  vars[1].init_2d(s_name + "[1]", my_grid);

  return 0;
}

PetscErrorCode  IceModelVec2Stag::begin_access() {
  PetscErrorCode ierr;
#ifdef PISM_DEBUG
  ierr = checkAllocated(); CHKERRQ(ierr);

  if (access_counter < 0)
    SETERRQ(1, "IceModelVec::begin_access(): access_counter < 0");
#endif

  if (access_counter == 0) {
    ierr = DAVecGetArrayDOF(da, v, &array); CHKERRQ(ierr);
  }

  access_counter++;

  return 0;
}

//! Checks if an IceModelVec is allocated and calls DAVecRestoreArray.
PetscErrorCode  IceModelVec2Stag::end_access() {
  PetscErrorCode ierr;
  access_counter--;

#ifdef PISM_DEBUG
  ierr = checkAllocated(); CHKERRQ(ierr);

  if (access_counter < 0)
    SETERRQ(1, "IceModelVec::end_access(): access_counter < 0");
#endif

  if (access_counter == 0) {
    ierr = DAVecRestoreArrayDOF(da, v, &array); CHKERRQ(ierr);
    array = NULL;
  }

  return 0;
}

PetscScalar& IceModelVec2Stag::operator() (int i, int j, int k) {
  check_array_indices(i, j);
  return static_cast<PetscScalar***>(array)[i][j][k];
}

PetscErrorCode IceModelVec2Stag::get_array(PetscScalar*** &a) {
  PetscErrorCode ierr;
  ierr = begin_access(); CHKERRQ(ierr);
  a = static_cast<PetscScalar***>(array);
  return 0;
}

//! Averages staggered grid values of a scalar field and puts them on a regular grid.
/*!
 * The current IceModelVec needs to have ghosts.
 */
PetscErrorCode IceModelVec2Stag::staggered_to_regular(IceModelVec2S &result) {
  PetscErrorCode ierr;

  ierr = result.begin_access(); CHKERRQ(ierr);
  ierr = begin_access(); CHKERRQ(ierr);
  for (PetscInt i = grid->xs; i < grid->xs+grid->xm; ++i) {
    for (PetscInt j = grid->ys; j < grid->ys+grid->ym; ++j) {
      result(i,j) = 0.25 * (  (*this)(i,j,0) + (*this)(i,j,1)
                              + (*this)(i,j-1,1) + (*this)(i-1,j,0) );
    } // j
  }   // i
  ierr = end_access(); CHKERRQ(ierr);
  ierr = result.end_access(); CHKERRQ(ierr);

  return 0;
}

//! \brief Averages staggered grid values of a 2D vector field (u on the
//! i-offset, v on the j-offset) and puts them on a regular grid.
/*!
 * The current IceModelVec needs to have ghosts.
 */
PetscErrorCode IceModelVec2Stag::staggered_to_regular(IceModelVec2V &result) {
  PetscErrorCode ierr;

  ierr = result.begin_access(); CHKERRQ(ierr);
  ierr = begin_access(); CHKERRQ(ierr);
  for (PetscInt i = grid->xs; i < grid->xs+grid->xm; ++i) {
    for (PetscInt j = grid->ys; j < grid->ys+grid->ym; ++j) {
        result(i,j).u = 0.5 * ((*this)(i-1,j,0) + (*this)(i,j,0));
        result(i,j).v = 0.5 * ((*this)(i,j-1,1) + (*this)(i,j,1));
    } // j
  }   // i
  ierr = end_access(); CHKERRQ(ierr);
  ierr = result.end_access(); CHKERRQ(ierr);

  return 0;
}

//! \brief Computes the norm of both components.
PetscErrorCode IceModelVec2Stag::norm_all(NormType n, PetscReal &result0, PetscReal &result1) {
  PetscErrorCode ierr;
  PetscReal *norm_result;

  norm_result = new PetscReal[dof];

  ierr = VecStrideNormAll(v, n, norm_result); CHKERRQ(ierr);

  if (localp) {
    // needs a reduce operation; use PetscGlobalMax if NORM_INFINITY,
    //   otherwise PetscGlobalSum; carefully in NORM_2 case
    if (n == NORM_1_AND_2) {
      SETERRQ1(1, 
         "IceModelVec2Stag::norm(...): NORM_1_AND_2 not implemented (called as %s.norm(...))\n",
         name.c_str());
    } else if (n == NORM_1) {
      ierr = PetscGlobalSum(&norm_result[0], &result0, grid->com); CHKERRQ(ierr);
      ierr = PetscGlobalSum(&norm_result[1], &result1, grid->com); CHKERRQ(ierr);
    } else if (n == NORM_2) {
      norm_result[0] = PetscSqr(norm_result[0]);  // undo sqrt in VecNorm before sum
      ierr = PetscGlobalSum(&norm_result[0], &result0, grid->com); CHKERRQ(ierr);
      result0 = sqrt(result0);

      norm_result[1] = PetscSqr(norm_result[1]);  // undo sqrt in VecNorm before sum
      ierr = PetscGlobalSum(&norm_result[1], &result1, grid->com); CHKERRQ(ierr);
      result1 = sqrt(result1);
    } else if (n == NORM_INFINITY) {
      ierr = PetscGlobalMax(&norm_result[0], &result0, grid->com); CHKERRQ(ierr);
      ierr = PetscGlobalMax(&norm_result[1], &result1, grid->com); CHKERRQ(ierr);
    } else {
      SETERRQ1(2, "IceModelVec::norm(...): unknown NormType (called as %s.norm(...))\n",
         name.c_str());
    }
  } else {
    result0 = norm_result[0];
    result1 = norm_result[1];
  }

  delete [] norm_result;

  return 0;
}
