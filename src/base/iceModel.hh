// Copyright (C) 2004-2011 Jed Brown, Ed Bueler and Constantine Khroulev
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

#ifndef __iceModel_hh
#define __iceModel_hh

//! \file iceModel.hh Definition of class IceModel.
/*! \file iceModel.hh
IceModel is a big class which is an ice flow model.  It contains all parts that
are not well-defined, separated components.  Such components are better places
to put sub-models that have a clear, general interface to the rest of an ice
sheet model.

IceModel has pointers to well-defined components, when they exist.

IceModel generally interprets user options, and initializes components based on
such options.  It manages the initialization sequences (%e.g. a restart from a
file containing a complete model state, versus bootstrapping).
 */

#include <signal.h>
#include <gsl/gsl_rng.h>
#include <petscsnes.h>
#include <petsctime.h>		// PetscGetTime()

#include "flowlaw_factory.hh"   // IceFlowLawFactory and friends
#include "basal_resistance.hh"  // IceBasalResistancePlasticLaw
#include "PISMYieldStress.hh"
#include "pism_const.hh"
#include "enthalpyConverter.hh"
#include "grid.hh"
#include "iceModelVec.hh"
#include "NCVariable.hh"
#include "PISMBedSmoother.hh"
#include "PISMVars.hh"
#include "Timeseries.hh"
#include "PISMStressBalance.hh"
#include "bedrockThermalUnit.hh"

#include "PISMBedDef.hh"
#include "PISMOcean.hh"
#include "PISMSurface.hh"

// use namespace std BUT remove trivial namespace browser from doxygen-erated HTML source browser
/// @cond NAMESPACE_BROWSER
using namespace std;
/// @endcond


//! The base class for PISM.  Contains all essential variables, parameters, and flags for modelling an ice sheet.
class IceModel {
  // The following classes implement various diagnostic computations.
  friend class IceModel_hardav;
  friend class IceModel_bwp;
  friend class IceModel_cts;
  friend class IceModel_dhdt;
  friend class IceModel_temp;
  friend class IceModel_temp_pa;
  friend class IceModel_temppabase;
  friend class IceModel_enthalpybase;
  friend class IceModel_enthalpysurf;
  friend class IceModel_tempbase;
  friend class IceModel_tempsurf;
  friend class IceModel_liqfrac;
  friend class IceModel_tempicethk;
  friend class IceModel_tempicethk_basal;
  friend class IceModel_new_mask;
public:
  // see iceModel.cc for implementation of constructor and destructor:
  IceModel(IceGrid &g, NCConfigVariable &config, NCConfigVariable &overrides);
  virtual ~IceModel(); // must be virtual merely because some members are virtual

  // see iMinit.cc
  virtual PetscErrorCode grid_setup();

  virtual PetscErrorCode allocate_submodels();
  virtual PetscErrorCode allocate_flowlaw();
  virtual PetscErrorCode allocate_enthalpy_converter();
  virtual PetscErrorCode allocate_basal_resistance_law();
  virtual PetscErrorCode allocate_stressbalance();
  virtual PetscErrorCode allocate_bed_deformation();
  virtual PetscErrorCode allocate_bedrock_thermal_unit();
  virtual PetscErrorCode allocate_basal_yield_stress();

  virtual PetscErrorCode init_couplers();
  virtual PetscErrorCode set_grid_from_options();
  virtual PetscErrorCode set_grid_defaults();
  virtual PetscErrorCode model_state_setup();
  virtual PetscErrorCode set_vars_from_options();
  virtual PetscErrorCode allocate_internal_objects();
  virtual PetscErrorCode misc_setup();
  virtual PetscErrorCode init_diagnostics();
  virtual PetscErrorCode init_ocean_kill();

  // see iceModel.cc
  PetscErrorCode init();
  virtual PetscErrorCode run();
  virtual PetscErrorCode step(bool do_mass_continuity, 
                              bool do_energy,
                              bool do_diffuse_bwat,
			      bool do_age,
			      bool do_skip);
  virtual PetscErrorCode setExecName(const char *my_executable_short_name);
  virtual IceFlowLawFactory &getIceFlowLawFactory() { return iceFactory; }
  virtual IceFlowLaw *getIceFlowLaw() {return ice;}
  virtual void reset_counters();

  // see iMbootstrap.cc 
  virtual PetscErrorCode bootstrapFromFile(const char *fname);
  virtual PetscErrorCode bootstrap_2d(const char *fname);
  virtual PetscErrorCode bootstrap_3d();
  virtual PetscErrorCode putTempAtDepth();

  // see iMoptions.cc
  virtual PetscErrorCode setFromOptions();
  virtual PetscErrorCode set_output_size(string option, string description,
					 string default_value, set<string> &result);

  // see iMutil.cc
  virtual void attach_surface_model(PISMSurfaceModel *surf);
  virtual void attach_ocean_model(PISMOceanModel *ocean);
  virtual PetscErrorCode additionalAtStartTimestep();
  virtual PetscErrorCode additionalAtEndTimestep();
  virtual PetscErrorCode compute_cell_areas(); // is an initialization step; should go there

  // see iMIO.cc
  virtual PetscErrorCode initFromFile(const char *);
  virtual PetscErrorCode writeFiles(const char* default_filename);
  virtual PetscErrorCode write_model_state(const char *filename);
  virtual PetscErrorCode write_metadata(const char *filename);
  virtual PetscErrorCode write_variables(const char* filename, set<string> vars,
					 nc_type nctype);
protected:

  IceGrid               &grid;

  NCConfigVariable      mapping, //!< grid projection (mapping) parameters
    &config,			 //!< configuration flags and parameters
    &overrides;			 //!< flags and parameters overriding config, see -config_override
  NCGlobalAttributes    global_attributes;

  IceFlowLawFactory     iceFactory;
  IceFlowLaw            *ice;

  PISMYieldStress *basal_yield_stress;
  IceBasalResistancePlasticLaw *basal;

  EnthalpyConverter *EC;
  PISMBedThermalUnit *btu;

  PISMSurfaceModel *surface;
  PISMOceanModel   *ocean;
  PISMBedDef       *beddef;

  //! \brief A dictionary with pointers to IceModelVecs below, for passing them
  //! from the IceModel core to other components (such as surface and ocean models)
  PISMVars variables;

  // state variables and some diagnostics/internals
  IceModelVec2S
        vh,		//!< ice surface elevation; ghosted
        vH,		//!< ice thickness; ghosted
        vdHdt,		//!< \f$ \frac{dH}{dt} \f$; ghosted to simplify the code computing it
        vtauc,		//!< yield stress for basal till (plastic or pseudo-plastic model); ghosted
        vHmelt,		//!< thickness of the basal meltwater; ghosted (because of the diffusion)
        vbmr,	    //!< rate of production of basal meltwater (ice-equivalent); no ghosts
        vLongitude,	//!< Longitude; ghosted to compute cell areas
        vLatitude,	//!< Latitude; ghosted to compute cell areas
        vbed,		//!< bed topography; ghosted
        vuplift,	//!< bed uplift rate; ghosted to simplify the code computing it
        vGhf,		//!< geothermal flux; no ghosts
        bedtoptemp,     //!< temperature seen by bedrock thermal layer, if present; no ghosts
                        //!< ghosted to be able to compute tauc "redundantly"

        vHref,          //!< accumulated mass advected to a partially filled grid cell
        vHresidual,     //!< residual ice mass of a not any longer partially (fully) filled grid cell
        vPrinStrain1,   //!< major principal component of horizontal strain-rate tensor
        vPrinStrain2,   //!< minor principal component of horizontal strain-rate tensor

    acab,		//!< accumulation/ablation rate; no ghosts
    artm,		//!< ice temperature at the ice surface but below firn; no ghosts
    liqfrac_surface,    //!< ice liquid water fraction at the top surface of the ice
    shelfbtemp,		//!< ice temperature at the shelf base; no ghosts
    shelfbmassflux,	//!< ice mass flux into the ocean at the shelf base; no ghosts
	cell_area;		//!< cell areas (computed using the WGS84 datum)

	
 
  IceModelVec2Int vMask, //!< \brief mask for flow type with values ice_free_bedrock,
                         //!< grounded_ice, floating_ice, ice_free_ocean
    ocean_kill_mask,     //!< mask used by the -ocean_kill code 
    vIcebergMask, //!< mask for iceberg identification

	vBCMask; //!< mask to determine Dirichlet boundary locations
 
  IceModelVec2V vBCvel; //!< Dirichlet boundary velocities


  IceModelVec3
        T3,		//!< absolute temperature of ice; K (ghosted)
        Enth3,          //!< enthalpy; J / kg (ghosted)
        tau3;		//!< age of ice; s (ghosted because it is evaluated on the staggered-grid)

  // parameters
  PetscReal   dt,     //!< mass continuity time step, s
              t_years_TempAge,  //!< time of last update for enthalpy/temperature
              dt_years_TempAge,  //!< enthalpy/temperature and age time-steps
              maxdt_temporary, dt_force,
              CFLviolcount,    //!< really is just a count, but PetscGlobalSum requires this type
              dt_from_diffus, dt_from_cfl, CFLmaxdt, CFLmaxdt2D,
              gDmax,		// global max of the diffusivity
              gmaxu, gmaxv, gmaxw,  // global maximums on 3D grid of abs value of vel components
              gdHdtav,  //!< average value in map-plane (2D) of dH/dt, where there is ice; m s-1
    total_sub_shelf_ice_flux,
    total_basal_ice_flux,
    total_surface_ice_flux,
    nonneg_rule_flux,
    ocean_kill_flux,
    float_kill_flux,
    dvoldt;  //!< d(total ice volume)/dt; m3 s-1
  PetscInt    skipCountDown;

  // physical parameters used frequently enough to make looking up via
  // config.get() a hassle:
  PetscScalar standard_gravity;
  // Initialized in the IceModel constructor from the configuration file;
  // SHOULD NOT be hard-wired.

  // flags
  PetscTruth  updateHmelt, shelvesDragToo, allowAboveMelting;
  PetscTruth  repeatRedist, putOnTop;
  char        adaptReasonFlag;

  string      stdout_flags, stdout_ssa;

  string executable_short_name;
  
protected:
  // see iceModel.cc
  virtual PetscErrorCode createVecs();
  virtual PetscErrorCode deallocate_internal_objects();

  // see iMadaptive.cc
  virtual PetscErrorCode computeMax3DVelocities();
  virtual PetscErrorCode computeMax2DSlidingSpeed();
  virtual PetscErrorCode adaptTimeStepDiffusivity();
  virtual PetscErrorCode determineTimeStep(const bool doTemperatureCFL);
  virtual PetscErrorCode countCFLViolations(PetscScalar* CFLviol);

  // see iMage.cc
  virtual PetscErrorCode ageStep();

  // see iMbeddef.cc
  PetscScalar last_bed_def_update;
  virtual PetscErrorCode bed_def_step(bool &bed_changed);

  // see iMcalving.cc
  virtual PetscErrorCode eigenCalving();
  virtual PetscErrorCode calvingAtThickness();

  // see iMdefaults.cc
  PetscErrorCode setDefaults();

  // see iMenergy.cc
  virtual PetscErrorCode energyStep();
  virtual PetscErrorCode get_bed_top_temp(IceModelVec2S &result);
  virtual bool checkThinNeigh(
       PetscScalar E, PetscScalar NE, PetscScalar N, PetscScalar NW, 
       PetscScalar W, PetscScalar SW, PetscScalar S, PetscScalar SE);

  // see iMenthalpy.cc
  virtual PetscErrorCode compute_enthalpy_cold(IceModelVec3 &temperature, IceModelVec3 &result);
  virtual PetscErrorCode compute_enthalpy(IceModelVec3 &temperature, IceModelVec3 &liquid_water_fraction,
                                          IceModelVec3 &result);
  virtual PetscErrorCode compute_liquid_water_fraction(IceModelVec3 &enthalpy, IceModelVec3 &result);

  virtual PetscErrorCode setCTSFromEnthalpy(IceModelVec3 &useForCTS);

  virtual PetscErrorCode getEnthalpyCTSColumn(PetscScalar p_air, //!< atmospheric pressure
					      PetscScalar thk,	 //!< ice thickness
					      PetscInt ks,	 //!< index of the level just below the surface
					      const PetscScalar *Enth, //!< Enthalpy
					      const PetscScalar *w,    //!< vert. velocity
					      PetscScalar *lambda,     //!< lambda
					      PetscScalar **Enth_s//!< enthalpy for the pr.-melting temp.
                                              );   
  virtual PetscErrorCode enthalpyAndDrainageStep(
                PetscScalar* vertSacrCount, PetscScalar* liquifiedVol,
                PetscScalar* bulgeCount);

  // see iMgeometry.cc
  virtual PetscErrorCode updateSurfaceElevationAndMask();
  virtual PetscErrorCode update_mask();
  virtual PetscErrorCode update_surface_elevation();
  virtual PetscErrorCode massContExplicitStep();

  // see iMhydrology.cc
  virtual PetscErrorCode diffuse_bwat();

  // see iMicebergs.cc
  virtual PetscErrorCode killIceBergs();           // call this one to do proper sequence
  virtual PetscErrorCode findIceBergCandidates();
  virtual PetscErrorCode identifyNotAnIceBerg();
  virtual PetscErrorCode killIdentifiedIceBergs();
  virtual PetscErrorCode killEasyIceBergs();       // FIXME: do we want this one to happen even if eigencalving does not happen?  should we be calling this one before any time that principal values need to be computed?

  // see iMIO.cc
  virtual PetscErrorCode set_time_from_options();
  virtual PetscErrorCode dumpToFile(const char *filename);
  virtual PetscErrorCode regrid(int dimensions);
  virtual PetscErrorCode regrid_variables(string filename, set<string> regrid_vars, int ndims);

  // see iMpartgrid.cc
  PetscErrorCode cell_interface_velocities(bool do_part_grid,
                                           int i, int j,
                                           planeStar<PetscScalar> &vel_output);
  PetscReal get_average_thickness(bool do_redist, planeStar<int> M,
                                  planeStar<PetscScalar> H);
  virtual PetscErrorCode redistResiduals();
  virtual PetscErrorCode calculateRedistResiduals();

  // see iMreport.cc
  virtual PetscErrorCode volumeArea(
                       PetscScalar& gvolume,PetscScalar& garea,
                       PetscScalar& gvolSIA, PetscScalar& gvolstream, 
                       PetscScalar& gvolshelf);
  virtual PetscErrorCode energyStats(
                       PetscScalar iarea,
                       PetscScalar &gmeltfrac, PetscScalar &gtemp0);
  virtual PetscErrorCode ageStats(PetscScalar ivol, PetscScalar &gorigfrac);
  virtual PetscErrorCode summary(bool tempAndAge);
  virtual PetscErrorCode summaryPrintLine(
              PetscTruth printPrototype, bool tempAndAge,
              PetscScalar year, PetscScalar delta_t, 
              PetscScalar volume, PetscScalar area,
              PetscScalar meltfrac, PetscScalar H0, PetscScalar T0);

  // see iMreport.cc;  methods for computing diagnostic quantities:
  // scalar:
  virtual PetscErrorCode ice_mass_bookkeeping();
  virtual PetscErrorCode compute_ice_volume(PetscScalar &result);
  virtual PetscErrorCode compute_ice_volume_temperate(PetscScalar &result);
  virtual PetscErrorCode compute_ice_volume_cold(PetscScalar &result);
  virtual PetscErrorCode compute_ice_area(PetscScalar &result);
  virtual PetscErrorCode compute_ice_area_temperate(PetscScalar &result);
  virtual PetscErrorCode compute_ice_area_cold(PetscScalar &result);
  virtual PetscErrorCode compute_ice_area_grounded(PetscScalar &result);
  virtual PetscErrorCode compute_ice_area_floating(PetscScalar &result);
  virtual PetscErrorCode compute_ice_enthalpy(PetscScalar &result);
  virtual PetscErrorCode compute_by_name(string name, PetscScalar &result);
  
  // see iMtemp.cc
  virtual PetscErrorCode excessToFromBasalMeltLayer(
                      const PetscScalar rho_c, const PetscScalar z, const PetscScalar dz,
                      PetscScalar *Texcess, PetscScalar *Hmelt);
  virtual PetscErrorCode temperatureStep(PetscScalar* vertSacrCount, PetscScalar* bulgeCount);

  // see iMutil.cc
  virtual int            endOfTimeStepHook();
  virtual PetscErrorCode stampHistoryCommand();
  virtual PetscErrorCode stampHistoryEnd();
  virtual PetscErrorCode stampHistory(string);
  virtual PetscErrorCode check_maximum_thickness();
  virtual PetscErrorCode check_maximum_thickness_hook(const int old_Mz);
  virtual bool           issounding(const PetscInt i, const PetscInt j);

protected:
  // working space (a convenience)
  static const PetscInt nWork2d=2;
  IceModelVec2S vWork2d[nWork2d];
  IceModelVec2V vWork2dV;

  // 3D working space
  IceModelVec3 vWork3d;

  PISMStressBalance *stress_balance;

  map<string,PISMDiagnostic*> diagnostics;

  // Set of variables to put in the output file:
  set<string> output_vars;

  // This is related to the snapshot saving feature
  string snapshots_filename;
  bool save_snapshots, snapshots_file_is_ready, split_snapshots;
  vector<double> snapshot_times;
  set<string> snapshot_vars;
  unsigned int current_snapshot;
  PetscErrorCode init_snapshots();
  PetscErrorCode write_snapshot();

  // scalar time-series
  bool save_ts;			//! true if the user requested time-series output
  string ts_filename;		//! file to write time-series to
  vector<double> ts_times;	//! times requested
  unsigned int current_ts;	//! index of the current time
  set<string> ts_vars;		//! variables requested
  vector<DiagnosticTimeseries*> timeseries;
  PetscErrorCode init_timeseries();
  PetscErrorCode create_timeseries();
  PetscErrorCode flush_timeseries();
  PetscErrorCode write_timeseries();
  PetscErrorCode ts_max_timestep(double t_years, double& dt_years);

  // spatially-varying time-series
  bool save_extra, extra_file_is_ready, split_extra;
  string extra_filename;
  vector<double> extra_times;
  unsigned int current_extra;
  set<string> extra_vars;
  PetscErrorCode init_extras();
  PetscErrorCode write_extras();
  PetscErrorCode extras_max_timestep(double t_years, double& dt_years);

  // automatic backups
  double backup_interval;
  string backup_filename;
  PetscReal last_backup_time;
  set<string> backup_vars;
  PetscErrorCode init_backups();
  PetscErrorCode write_backup();

  // diagnostic viewers; see iMviewers.cc
  virtual PetscErrorCode init_viewers();
  virtual PetscErrorCode update_viewers();
  set<string> map_viewers, slice_viewers, sounding_viewers;
  PetscInt     id, jd;	     // sounding indices
  map<string,PetscViewer> viewers;

private:
  PetscLogDouble start_time;    // this is used in the wall-clock-time backup code

  int event_step,		//!< total time spent doing time-stepping
    event_velocity,		//!< total velocity computation
    event_energy,		//!< energy balance computation
    event_mass,			//!< mass continuity computation
    event_age,			//!< age computation
    event_beddef,		//!< bed deformation step
    event_output,		//!< time spent writing the output file
    event_snapshots,            //!< time spent writing snapshots
    event_backups;              //!< time spent writing backups files
};

#endif /* __iceModel_hh */

