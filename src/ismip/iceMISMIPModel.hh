// Copyright (C) 2008 Ed Bueler
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

#ifndef __iceMISMIPModel_hh
#define __iceMISMIPModel_hh

#include <petscvec.h>
#include "../base/grid.hh"
#include "../base/materials.hh"
#include "../base/iceModel.hh"

class IceMISMIPModel : public IceModel {

public:
  IceMISMIPModel(IceGrid &g, IceType &i);
  virtual PetscErrorCode setFromOptions();
  virtual PetscErrorCode initFromOptions();

protected:
  virtual PetscErrorCode additionalAtStartTimestep();

private:
  PetscTruth inFileSet;
};

#endif  // __iceMISMIPModel_hh

