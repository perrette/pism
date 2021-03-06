// Copyright (C) 2009--2011 Constantine Khroulev
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

#ifndef __NCSpatialVariable
#define __NCSpatialVariable

#include "NCVariable.hh"
#include "LocalInterpCtx.hh"

//! Spatial NetCDF variable (corresponding to a 2D or 3D scalar field).
class NCSpatialVariable : public NCVariable {
public:
  NCSpatialVariable();
  virtual void init_2d(string name, IceGrid &g);
  virtual void init_3d(string name, IceGrid &g, vector<double> &zlevels);
  virtual void set_levels(const vector<double> &levels);
  virtual PetscErrorCode read(string filename, unsigned int time, Vec v);
  virtual PetscErrorCode reset();
  virtual PetscErrorCode write(string filename, nc_type nctype,
			       bool write_in_glaciological_units, Vec v);
  virtual PetscErrorCode regrid(string filename, LocalInterpCtx *lic,
				bool critical, bool set_default_value,
				PetscScalar default_value, Vec v);
  virtual PetscErrorCode to_glaciological_units(Vec v);

  PetscErrorCode define(const NCTool &nc, int &varid, nc_type nctype,
                        bool write_in_glaciological_units) const;

  mutable map<string,string> dimensions,
    x_attrs, y_attrs, z_attrs;
  bool time_independent;        //!< a variable in a NetCDF file will not
                                //! depend on 't' if this is true.
protected:
  int nlevels;
  vector<double> zlevels;
  IceGrid *grid;
  PetscErrorCode report_range(Vec v, bool found_by_standard_name);
  PetscErrorCode change_units(Vec v, utUnit *from, utUnit *to);
  PetscErrorCode check_range(Vec v);
  PetscErrorCode define_dimensions(const NCTool &nc) const;
};

#endif	// __NCSpatialVariable
