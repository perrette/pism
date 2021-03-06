// Copyright (C) 2004-2011 Jed Brown, Ed Bueler, and Constantine Khroulev
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

#include "flowlaws.hh"
#include "pism_const.hh"
#include "enthalpyConverter.hh"


PetscTruth IceFlowLawUsesGrainSize(IceFlowLaw *ice) {
  static const PetscReal gs[] = {1e-4,1e-3,1e-2,1},s=1e4,E=500000,p=1e6;
  PetscReal ref = ice->flow_from_enth(s,E,p,gs[0]);
  for (int i=1; i<4; i++) {
    if (ice->flow_from_enth(s,E,p,gs[i]) != ref) return PETSC_TRUE;
  }
  return PETSC_FALSE;
}

// Rather than make this part of the base class, we just check at some reference values.
PetscTruth IceFlowLawIsPatersonBuddCold(IceFlowLaw *ice, const NCConfigVariable &config) {
  static const struct {PetscReal s,E,p,gs;} v[] = {
    {1e3,223,1e6,1e-3},{450000,475000,500000,525000},{5e4,268,5e6,3e-3},{1e5,273,8e6,5e-3}};
  ThermoGlenArrIce cpb(PETSC_COMM_SELF,NULL,config); // This is unmodified cold Paterson-Budd
  for (int i=0; i<4; i++) {
    const PetscReal left  = ice->flow_from_enth(v[i].s, v[i].E, v[i].p, v[i].gs),
                    right =  cpb.flow_from_enth(v[i].s, v[i].E, v[i].p, v[i].gs);
    if (PetscAbs((left - right)/left)>1.0e-15) {
      return PETSC_FALSE;
    }
  }
  return PETSC_TRUE;
}

IceFlowLaw::IceFlowLaw(MPI_Comm c,const char pre[], const NCConfigVariable &config) : comm(c) {
  PetscMemzero(prefix,sizeof(prefix));
  if (pre) PetscStrncpy(prefix,pre,sizeof(prefix));

  standard_gravity   = config.get("standard_gravity");
  ideal_gas_constant = config.get("ideal_gas_constant");

  rho          = config.get("ice_density");
  beta_CC_grad = config.get("beta_CC") * rho * standard_gravity;
  k            = config.get("ice_thermal_conductivity");
  c_p          = config.get("ice_specific_heat_capacity");
  latentHeat   = config.get("water_latent_heat_fusion");
  melting_point_temp = config.get("water_melting_point_temperature");
  n            = config.get("Glen_exponent");

  A_cold = config.get("Paterson-Budd_A_cold");
  A_warm = config.get("Paterson-Budd_A_warm");
  Q_cold = config.get("Paterson-Budd_Q_cold");
  Q_warm = config.get("Paterson-Budd_Q_warm");
  crit_temp = config.get("Paterson-Budd_critical_temperature");
  schoofLen = config.get("Schoof_regularizing_length") * 1e3; // convert to meters
  schoofVel = config.get("Schoof_regularizing_velocity")/secpera; // convert to m/s
  schoofReg = PetscSqr(schoofVel/schoofLen);

  if (config.get_flag("verification_mode")) {
    EC = new ICMEnthalpyConverter(config);
  } else {
    EC = new EnthalpyConverter(config);
  }
}

IceFlowLaw::~IceFlowLaw() {
  delete EC;
}

PetscErrorCode IceFlowLaw::setFromOptions() {
  PetscErrorCode ierr;
  PetscReal slen=schoofLen/1e3,	// convert to km
    svel=schoofVel*secpera;	// convert to m/year

  ierr = PetscOptionsBegin(comm,prefix,"IceFlowLaw options",NULL); CHKERRQ(ierr);
  {
    ierr = PetscOptionsReal("-ice_reg_schoof_vel",
                            "Regularizing velocity (Schoof definition, m/a)",
                            "",svel,&svel,NULL);CHKERRQ(ierr);

    ierr = PetscOptionsReal("-ice_reg_schoof_length",
                            "Regularizing length (Schoof definition, km)",
                            "",slen,&slen,NULL);CHKERRQ(ierr);

    schoofVel = svel / secpera;	// convert to m/s
    schoofLen = slen * 1e3;	// convert to meters
    schoofReg = PetscSqr(schoofVel/schoofLen);

    ierr = PetscOptionsReal("-ice_pb_A_cold",
                            "Paterson-Budd cold softness parameter (Pa^-3 s^-1)",
                            "",A_cold,&A_cold,NULL);CHKERRQ(ierr);

    ierr = PetscOptionsReal("-ice_pb_A_warm",
                            "Paterson-Budd warm softness parameter (Pa^-3 s^-1)",
                            "",A_warm,&A_warm,NULL);CHKERRQ(ierr);

    ierr = PetscOptionsReal("-ice_pb_Q_cold",
                            "Paterson-Budd activation energy (J/mol)",
                            "",Q_cold,&Q_cold,NULL);CHKERRQ(ierr);

    ierr = PetscOptionsReal("-ice_pb_Q_warm",
                            "Paterson-Budd activation energy (J/mol)",
                            "",Q_warm,&Q_warm,NULL);CHKERRQ(ierr);

    ierr = PetscOptionsReal("-ice_pb_crit_temp",
                            "Paterson-Budd critical temperature (K)",
                            "",crit_temp,&crit_temp,NULL);CHKERRQ(ierr);
  }
  ierr = PetscOptionsEnd(); CHKERRQ(ierr);
  return 0;
}

//! Returns viscosity and \b not the nu * H product.
PetscReal IceFlowLaw::effectiveViscosity(PetscReal hardness,
                                         PetscReal u_x, PetscReal u_y,
                                         PetscReal v_x, PetscReal v_y) const {
  const PetscReal alpha = secondInvariant(u_x, u_y, v_x, v_y);
  return 0.5 * hardness * pow(schoofReg + alpha, (1-n)/(2*n));
}

//! \brief Computes effective viscosity and its derivative with respect to the
//! second invariant.
void IceFlowLaw::effectiveViscosity_with_derivative(PetscReal hardness, const PetscReal Du[],
                                                    PetscReal *nu, PetscReal *dnu) const {

  const PetscReal alpha = secondInvariantDu(Du),
    power = (1-n)/(2*n),
    my_nu = 0.5 * hardness * pow(schoofReg + alpha, power),
    my_dnu = power * my_nu / (schoofReg + alpha);

  if (nu)   *nu = my_nu;
  if (dnu) *dnu = my_dnu;
}

//! Return the softness parameter A(T) for a given temperature T.
/*! This is not a natural part of all IceFlowLaw instances.   */
PetscReal IceFlowLaw::softnessParameter_paterson_budd(PetscReal T_pa) const {
  if (T_pa < crit_temp) {
    return A_cold * exp(-Q_cold/(ideal_gas_constant * T_pa));
  }
  return A_warm * exp(-Q_warm/(ideal_gas_constant * T_pa));
}

//! The flow law itself.
PetscReal IceFlowLaw::flow_from_enth(PetscReal stress, PetscReal enthalpy,
                                       PetscReal pressure, PetscReal /* gs */) const {
  return softnessParameter_from_enth(enthalpy,pressure) * pow(stress,n-1);
}

PetscReal IceFlowLaw::hardnessParameter_from_enth(PetscReal E, PetscReal p) const {
  return pow(softnessParameter_from_enth(E, p), -1.0/n);
}

//! Computes vertical average of B(E,pressure) ice hardness, namely \f$\bar
//! B(E,p)\f$. See comment for hardnessParameter().
/*! Note E[0],...,E[kbelowH] must be valid.  */
PetscReal IceFlowLaw::averagedHardness_from_enth(PetscReal thickness, PetscInt kbelowH,
                                                 const PetscReal *zlevels,
                                                 const PetscReal *enthalpy) const {
  PetscReal B = 0;

  // Use trapezoidal rule to integrate from 0 to zlevels[kbelowH]:
  if (kbelowH > 0) {
    PetscReal
      p0 = EC->getPressureFromDepth(thickness),
      E0 = enthalpy[0],
      h0 = hardnessParameter_from_enth(E0, p0); // ice hardness at the left endpoint

    for (int i = 1; i <= kbelowH; ++i) { // note the "1" and the "<="
      const PetscReal
        p1 = EC->getPressureFromDepth(thickness - zlevels[i]), // pressure at the right endpoint
        E1 = enthalpy[i],       // enthalpy at the right endpoint
        h1 = hardnessParameter_from_enth(E1, p1); // ice hardness at the right endpoint

      // The midpoint rule sans the "1/2":
      B += (zlevels[i] - zlevels[i-1]) * (h0 + h1);

      h0 = h1;
    }
  }

  // Add the "1/2":
  B *= 0.5;

  // use the "rectangle method" to integrate from
  // zlevels[kbelowH] to thickness:
  PetscReal
    depth = thickness - zlevels[kbelowH],
    p = EC->getPressureFromDepth(depth);

  B += depth * hardnessParameter_from_enth(enthalpy[kbelowH], p);

  // Now B is an integral of ice hardness; next, compute the average:
  if (thickness > 0)
    B = B / thickness;
  else
    B = 0;

  return B;
}


/*!
This constructor just sets flow law factor for nonzero water content, from
\ref AschwandenBlatter and \ref LliboutryDuval1985.
 */
GPBLDIce::GPBLDIce(MPI_Comm c,const char pre[],
                   const NCConfigVariable &config) : IceFlowLaw(c,pre,config) {
  T_0              = config.get("water_melting_point_temperature");    // K
  water_frac_coeff = config.get("gpbld_water_frac_coeff");
  water_frac_observed_limit
                   = config.get("gpbld_water_frac_observed_limit");
}

PetscErrorCode GPBLDIce::setFromOptions() {
  PetscErrorCode ierr;

  ierr = IceFlowLaw::setFromOptions(); CHKERRQ(ierr);

  ierr = PetscOptionsBegin(comm,prefix,"GPBLDIce options",NULL); CHKERRQ(ierr);
  {
    ierr = PetscOptionsReal("-ice_gpbld_water_frac_coeff",
                            "coefficient of softness factor in temperate ice,"
                            " as function of liquid water fraction; no units",
                            "",water_frac_coeff,&water_frac_coeff,NULL); CHKERRQ(ierr);
    ierr = PetscOptionsReal("-ice_gpbld_water_frac_observed_limit",
                            "maximum value of liquid water fraction 'omega' for"
                            " which softness values are parameterized by Lliboutry and"
                            " Duval (1985); no units",
                            "",water_frac_observed_limit,&water_frac_observed_limit,NULL);
                            CHKERRQ(ierr);
  }
  ierr = PetscOptionsEnd(); CHKERRQ(ierr);
  return 0;
}

//! The softness factor in the Glen-Paterson-Budd-Lliboutry-Duval flow law.  For constitutive law form.
/*!
This is a modification of Glen-Paterson-Budd ice, which is ThermoGlenIce.  In particular, if
\f$A()\f$ is the softness factor for ThermoGlenIce, if \f$E\f$ is the enthalpy, and \f$p\f$ is
the pressure then the softness we compute is
   \f[A = A(T_{pa}(E,p))(1+184\omega).\f]
The pressure-melting temperature \f$T_{pa}(E,p)\f$ is computed by getPATemp().
 */
PetscReal GPBLDIce::softnessParameter_from_enth(
                PetscReal enthalpy, PetscReal pressure) const {
  PetscErrorCode ierr;

  if (EC == NULL) {
    PetscErrorPrintf("EC is NULL in GPBLDIce::softnessParameter_from_enth()\n");
    endPrintRank();
  }
  PetscReal E_s, E_l;
  EC->getEnthalpyInterval(pressure, E_s, E_l);
  if (enthalpy < E_s) {       // cold ice
    PetscReal T_pa;
    ierr = EC->getPATemp(enthalpy,pressure,T_pa);
    if (ierr) {
      PetscErrorPrintf(
        "getPATemp() returned ierr>0 in GPBLDIce::softnessParameter_from_enth()\n");
      endPrintRank();
    }
    return softnessParameter_paterson_budd( T_pa );
  } else { // temperate ice
    PetscReal omega;
    ierr = EC->getWaterFraction(enthalpy,pressure,omega);
    // as stated in \ref AschwandenBuelerBlatter, cap omega at max of observations:
    omega = PetscMin(omega,water_frac_observed_limit);
    // next line implements eqn (23) in \ref AschwandenBlatter2009
    return softnessParameter_paterson_budd(T_0) * (1.0 + water_frac_coeff * omega);
  }
}

// ThermoGlenIce

/*! Converts enthalpy to temperature and uses the Paterson-Budd formula. */
PetscReal ThermoGlenIce::softnessParameter_from_enth(PetscReal E, PetscReal pressure) const {
  PetscReal T_pa;
  EC->getPATemp(E, pressure, T_pa);
  return softnessParameter_from_temp(T_pa);
}

/*! Converts enthalpy to temperature and calls flow_from_temp. */
PetscReal ThermoGlenIce::flow_from_enth(PetscReal stress, PetscReal E,
                                        PetscReal pressure, PetscReal gs) const {
  PetscReal temp;
  EC->getAbsTemp(E, pressure, temp);
  return flow_from_temp(stress, temp, pressure, gs);
}

//! The flow law (temperature-dependent version).
PetscReal ThermoGlenIce::flow_from_temp(PetscReal stress, PetscReal temp,
                                        PetscReal pressure, PetscReal /*gs*/) const {
  // pressure-adjusted temperature:
  const PetscReal T_pa = temp + (beta_CC_grad / (rho * standard_gravity)) * pressure;
  return softnessParameter_from_temp(T_pa) * pow(stress,n-1);
}

// CustomGlenIce

CustomGlenIce::CustomGlenIce(MPI_Comm c, const char pre[], const NCConfigVariable &config)
  : ThermoGlenIce(c, pre, config) {
  softness_A = config.get("ice_softness");
  hardness_B = pow(softness_A, -1/n);
}

void CustomGlenIce::setHardness(PetscReal hardness) {
  hardness_B = hardness;
  softness_A = pow(hardness_B,-n);
}

void CustomGlenIce::setSoftness(PetscReal softness) {
  softness_A = softness;
  hardness_B = pow(softness_A, -1/n);
}

void CustomGlenIce::setExponent(PetscReal new_n) { n = new_n; }
void CustomGlenIce::setDensity(PetscReal density) { rho = density; }

void CustomGlenIce::setSchoofRegularization(PetscReal vel_peryear,
                                            PetscReal len_km) {
  schoofVel = vel_peryear/secpera;
  schoofLen = len_km*1e3;
  schoofReg = PetscSqr(schoofVel/schoofLen);
}

// HookeIce

HookeIce::HookeIce(MPI_Comm c, const char pre[], const NCConfigVariable &config)
 : ThermoGlenIce(c, pre, config) {
  Q_Hooke  = config.get("Hooke_Q");
  A_Hooke  = config.get("Hooke_A");
  C_Hooke  = config.get("Hooke_C");
  K_Hooke  = config.get("Hooke_k");
  Tr_Hooke = config.get("Hooke_Tr");
}

PetscReal HookeIce::softnessParameter_from_temp(PetscReal T_pa) const {
  return A_Hooke * exp( -Q_Hooke/(ideal_gas_constant * T_pa)
                        + 3.0 * C_Hooke * pow(Tr_Hooke - T_pa, -K_Hooke));
}

// Hybrid (Goldsby-Kohlstedt / Glen) ice flow law

HybridIce::HybridIce(MPI_Comm c,const char pre[],
		     const NCConfigVariable &config) : ThermoGlenIce(c,pre,config) {

  V_act_vol    = -13.e-6;  // m^3/mol
  d_grain_size = 1.0e-3;   // m  (see p. ?? of G&K paper)
  //--- dislocation creep ---
  disl_crit_temp=258.0,    // Kelvin
  //disl_A_cold=4.0e5,                  // MPa^{-4.0} s^{-1}
  //disl_A_warm=6.0e28,                 // MPa^{-4.0} s^{-1}
  disl_A_cold=4.0e-19,     // Pa^{-4.0} s^{-1}
  disl_A_warm=6.0e4,       // Pa^{-4.0} s^{-1} (GK)
  disl_n=4.0,              // stress exponent
  disl_Q_cold=60.e3,       // J/mol Activation energy
  disl_Q_warm=180.e3;      // J/mol Activation energy (GK)
  //--- grain boundary sliding ---
  gbs_crit_temp=255.0,     // Kelvin
  //gbs_A_cold=3.9e-3,                  // MPa^{-1.8} m^{1.4} s^{-1}
  //gbs_A_warm=3.e26,                   // MPa^{-1.8} m^{1.4} s^{-1}
  gbs_A_cold=6.1811e-14,   // Pa^{-1.8} m^{1.4} s^{-1}
  gbs_A_warm=4.7547e15,    // Pa^{-1.8} m^{1.4} s^{-1}
  gbs_n=1.8,               // stress exponent
  gbs_Q_cold=49.e3,        // J/mol Activation energy
  gbs_Q_warm=192.e3,       // J/mol Activation energy
  p_grain_sz_exp=1.4;      // from Peltier
  //--- easy slip (basal) ---
  //basal_A=5.5e7,                      // MPa^{-2.4} s^{-1}
  basal_A=2.1896e-7,       // Pa^{-2.4} s^{-1}
  basal_n=2.4,             // stress exponent
  basal_Q=60.e3;           // J/mol Activation energy
  //--- diffusional flow ---
  diff_crit_temp=258.0,    // when to use enhancement factor
  diff_V_m=1.97e-5,        // Molar volume (m^3/mol)
  diff_D_0v=9.10e-4,       // Preexponential volume diffusion (m^2/s)
  diff_Q_v=59.4e3,         // activation energy, vol. diff. (J/mol)
  diff_D_0b=5.8e-4,        // preexponential grain boundary coeff.
  diff_Q_b=49.e3,          // activation energy, g.b. (J/mol)
  diff_delta=9.04e-10;     // grain boundary width (m)
}


/*!
  This is the (forward) Goldsby-Kohlstedt flow law.  See:
  D. L. Goldsby & D. L. Kohlstedt (2001), "Superplastic deformation
  of ice: experimental observations", J. Geophys. Res. 106(M6), 11017-11030.
*/
PetscReal HybridIce::flow_from_temp(PetscReal stress, PetscReal temp,
                                    PetscReal pressure, PetscReal gs) const {
  PetscReal eps_diff, eps_disl, eps_basal, eps_gbs, diff_D_b;

  if (PetscAbs(stress) < 1e-10) return 0;
  const PetscReal T = temp + (beta_CC_grad / (rho * standard_gravity)) * pressure;
  const PetscReal pV = pressure * V_act_vol;
  const PetscReal RT = ideal_gas_constant * T;
  // Diffusional Flow
  const PetscReal diff_D_v = diff_D_0v * exp(-diff_Q_v/RT);
  diff_D_b = diff_D_0b * exp(-diff_Q_b/RT);
  if (T > diff_crit_temp) diff_D_b *= 1000; // Coble creep scaling
  eps_diff = 14 * diff_V_m *
    (diff_D_v + M_PI * diff_delta * diff_D_b / gs) / (RT*PetscSqr(gs));
  // Dislocation Creep
  if (T > disl_crit_temp)
    eps_disl = disl_A_warm * pow(stress, disl_n-1) * exp(-(disl_Q_warm + pV)/RT);
  else
    eps_disl = disl_A_cold * pow(stress, disl_n-1) * exp(-(disl_Q_cold + pV)/RT);
  // Basal Slip
  eps_basal = basal_A * pow(stress, basal_n-1) * exp(-(basal_Q + pV)/RT);
  // Grain Boundary Sliding
  if (T > gbs_crit_temp)
    eps_gbs = gbs_A_warm * (pow(stress, gbs_n-1) / pow(gs, p_grain_sz_exp)) *
      exp(-(gbs_Q_warm + pV)/RT);
  else
    eps_gbs = gbs_A_cold * (pow(stress, gbs_n-1) / pow(gs, p_grain_sz_exp)) *
      exp(-(gbs_Q_cold + pV)/RT);

  return eps_diff + eps_disl + (eps_basal * eps_gbs) / (eps_basal + eps_gbs);
}


/*****************
THE NEXT PROCEDURE REPEATS CODE; INTENDED ONLY FOR DEBUGGING
*****************/
GKparts HybridIce::flowParts(PetscReal stress,PetscReal temp,PetscReal pressure) const {
  PetscReal gs, eps_diff, eps_disl, eps_basal, eps_gbs, diff_D_b;
  GKparts p;

  if (PetscAbs(stress) < 1e-10) {
    p.eps_total=0.0;
    p.eps_diff=0.0; p.eps_disl=0.0; p.eps_gbs=0.0; p.eps_basal=0.0;
    return p;
  }
  const PetscReal T = temp + (beta_CC_grad / (rho * standard_gravity)) * pressure;
  const PetscReal pV = pressure * V_act_vol;
  const PetscReal RT = ideal_gas_constant * T;
  // Diffusional Flow
  const PetscReal diff_D_v = diff_D_0v * exp(-diff_Q_v/RT);
  diff_D_b = diff_D_0b * exp(-diff_Q_b/RT);
  if (T > diff_crit_temp) diff_D_b *= 1000; // Coble creep scaling
  gs = d_grain_size;
  eps_diff = 14 * diff_V_m *
    (diff_D_v + M_PI * diff_delta * diff_D_b / gs) / (RT*PetscSqr(gs));
  // Dislocation Creep
  if (T > disl_crit_temp)
    eps_disl = disl_A_warm * pow(stress, disl_n-1) * exp(-(disl_Q_warm + pV)/RT);
  else
    eps_disl = disl_A_cold * pow(stress, disl_n-1) * exp(-(disl_Q_cold + pV)/RT);
  // Basal Slip
  eps_basal = basal_A * pow(stress, basal_n-1) * exp(-(basal_Q + pV)/RT);
  // Grain Boundary Sliding
  if (T > gbs_crit_temp)
    eps_gbs = gbs_A_warm * (pow(stress, gbs_n-1) / pow(gs, p_grain_sz_exp)) *
      exp(-(gbs_Q_warm + pV)/RT);
  else
    eps_gbs = gbs_A_cold * (pow(stress, gbs_n-1) / pow(gs, p_grain_sz_exp)) *
      exp(-(gbs_Q_cold + pV)/RT);

  p.eps_diff=eps_diff;
  p.eps_disl=eps_disl;
  p.eps_basal=eps_basal;
  p.eps_gbs=eps_gbs;
  p.eps_total=eps_diff + eps_disl + (eps_basal * eps_gbs) / (eps_basal + eps_gbs);
  return p;
}
/*****************/

HybridIceStripped::HybridIceStripped(MPI_Comm c,const char pre[],
				     const NCConfigVariable &config) : HybridIce(c,pre,config) {
  d_grain_size_stripped = 3.0e-3;  // m; = 3mm  (see Peltier et al 2000 paper)
}


PetscReal HybridIceStripped::flow_from_temp(PetscReal stress, PetscReal temp, PetscReal pressure, PetscReal) const {
  // note value of gs is ignored
  // note pressure only effects the temperature; the "P V" term is dropped
  // note no diffusional flow
  PetscReal eps_disl, eps_basal, eps_gbs;

  if (PetscAbs(stress) < 1e-10) return 0;
  const PetscReal T = temp + (beta_CC_grad / (rho * standard_gravity)) * pressure;
  const PetscReal RT = ideal_gas_constant * T;
  // NO Diffusional Flow
  // Dislocation Creep
  if (T > disl_crit_temp)
    eps_disl = disl_A_warm * pow(stress, disl_n-1) * exp(-disl_Q_warm/RT);
  else
    eps_disl = disl_A_cold * pow(stress, disl_n-1) * exp(-disl_Q_cold/RT);
  // Basal Slip
  eps_basal = basal_A * pow(stress, basal_n-1) * exp(-basal_Q/RT);
  // Grain Boundary Sliding
  if (T > gbs_crit_temp)
    eps_gbs = gbs_A_warm *
              (pow(stress, gbs_n-1) / pow(d_grain_size_stripped, p_grain_sz_exp)) *
              exp(-gbs_Q_warm/RT);
  else
    eps_gbs = gbs_A_cold *
              (pow(stress, gbs_n-1) / pow(d_grain_size_stripped, p_grain_sz_exp)) *
              exp(-gbs_Q_cold/RT);

  return eps_disl + (eps_basal * eps_gbs) / (eps_basal + eps_gbs);
}
