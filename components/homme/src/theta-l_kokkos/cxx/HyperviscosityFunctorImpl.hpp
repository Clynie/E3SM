/********************************************************************************
 * HOMMEXX 1.0: Copyright of Sandia Corporation
 * This software is released under the BSD license
 * See the file 'COPYRIGHT' in the HOMMEXX/src/share/cxx directory
 *******************************************************************************/

#ifndef HOMMEXX_HYPERVISCOSITY_FUNCTOR_IMPL_HPP
#define HOMMEXX_HYPERVISCOSITY_FUNCTOR_IMPL_HPP

#include "Elements.hpp"
#include "ColumnOps.hpp"
#include "EquationOfState.hpp"
#include "ElementOps.hpp"
#include "HybridVCoord.hpp"
#include "KernelVariables.hpp"
#include "SimulationParams.hpp"
#include "SphereOperators.hpp"

#include "utilities/VectorUtils.hpp"

#include <memory>

namespace Homme
{

class BoundaryExchange;
struct FunctorsBuffersManager;

class HyperviscosityFunctorImpl
{
  // TODO: don't pass nu_ratio1/2. Instead, do like in F90: compute them from
  //       nu, nu_div, and hv_scaling
  struct HyperviscosityData {
    HyperviscosityData(const int hypervis_subcycle_in, const Real nu_ratio1_in, const Real nu_ratio2_in, const Real nu_top_in,
                       const Real nu_in, const Real nu_p_in, const Real nu_s_in,
                       const Real hypervis_scaling_in)
                      : hypervis_subcycle(hypervis_subcycle_in), nu_ratio1(nu_ratio1_in), nu_ratio2(nu_ratio2_in)
                      , nu_top(nu_top_in), nu(nu_in), nu_p(nu_p_in), nu_s(nu_s_in)
                      , hypervis_scaling(hypervis_scaling_in)
                      , consthv(hypervis_scaling_in == 0){}

    const int   hypervis_subcycle;

    Real  nu_ratio1;
    Real  nu_ratio2;

    const Real  nu_top;
    const Real  nu;
    const Real  nu_p;
    const Real  nu_s;

    int         np1; // The time-level on which to apply hv
    Real        dt;

    Real        eta_ave_w;

    Real hypervis_scaling;
    bool consthv;
  };

  static constexpr int NUM_BIHARMONIC_PHYSICAL_LEVELS = 3;
  static constexpr int NUM_BIHARMONIC_LEV = ColInfo<NUM_BIHARMONIC_PHYSICAL_LEVELS>::NumPacks;

  struct Buffers {
    ExecViewManaged<Real * [NP][NP]> ps_ref;

    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]> dp_ref;
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]> p;
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]> theta_ref;

    ExecViewManaged<Scalar * [NP][NP][NUM_LEV_P]> p_i;
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV_P]> phi_i_ref;

    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]>    dptens;
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]>    ttens;
#ifdef XX_NONBFB_COMING
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV_P]>  wtens;
#else
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]>    wtens;
#endif
    ExecViewManaged<Scalar * [NP][NP][NUM_LEV]>    phitens;
    ExecViewManaged<Scalar * [2][NP][NP][NUM_LEV]> vtens;

    ExecViewManaged<Scalar * [NP][NP][NUM_BIHARMONIC_LEV]>    lapl_dp;
    ExecViewManaged<Scalar * [NP][NP][NUM_BIHARMONIC_LEV]>    lapl_theta;
    ExecViewManaged<Scalar * [NP][NP][NUM_BIHARMONIC_LEV]>    lapl_w;
    ExecViewManaged<Scalar * [NP][NP][NUM_BIHARMONIC_LEV]>    lapl_phi;
    ExecViewManaged<Scalar * [2][NP][NP][NUM_BIHARMONIC_LEV]> lapl_v;
  };

  static constexpr Real Rgas = PhysicalConstants::Rgas;
  static constexpr Real kappa = PhysicalConstants::kappa;

public:

  struct TagRefStates {};

  struct TagFirstLaplaceHV {};
  struct TagSecondLaplaceConstHV {};
  struct TagSecondLaplaceTensorHV {};
  struct TagUpdateStates {};
  struct TagApplyInvMass {};
  struct TagHyperPreExchange {};

  HyperviscosityFunctorImpl (const SimulationParams& params,
                             const Elements&         elements);

  int requested_buffer_size () const;
  void init_buffers (const FunctorsBuffersManager& fbm);
  void init_boundary_exchanges();

  void run (const int np1, const Real dt, const Real eta_ave_w);

  void biharmonic_wk_theta () const;

  // first iter of laplace, const hv
  KOKKOS_INLINE_FUNCTION
  void operator() (const TagRefStates&, const TeamMember& team) const {
    KernelVariables kv(team);

    Kokkos::parallel_for(Kokkos::TeamThreadRange(kv.team,NP*NP),
                         [&](const int idx) {
      const int igp = idx / NP;
      const int jgp = idx % NP;

      auto dp    = Homme::subview(m_buffers.dp_ref,kv.ie,igp,jgp);
      auto p     = Homme::subview(m_buffers.p,kv.ie,igp,jgp);
      auto p_i   = Homme::subview(m_buffers.p_i,kv.team_idx,igp,jgp);
      auto phi_i = Homme::subview(m_buffers.phi_i_ref,kv.ie,igp,jgp);
      auto theta = Homme::subview(m_buffers.theta_ref,kv.ie,igp,jgp);

      // Step 0: compute ps_ref
      const Real ps_ref = m_hvcoord.ps0 * exp(-m_geometry.m_phis(kv.ie,igp,jgp)/(Rgas*300));

      // Step 1: compute dp_ref = delta_hyai(k)*ps0 + delta_hybi(k)*ps_ref
      m_hvcoord.compute_dp_ref(kv,ps_ref,dp);

      // Step 2: compute p_ref = p(p_i(dp))
      p_i(0)[0] = m_hvcoord.hybrid_ai0*m_hvcoord.ps0;
      m_col_ops.column_scan_mid_to_int<true>(kv,dp,p_i);
      m_col_ops.compute_midpoint_values(kv,p_i,p);

      // Step 3: compute theta_ref = theta(exner(p_ref))
      m_elem_ops.compute_theta_ref(kv,p,theta);

      // Step 3: compute phi_i_ref = phi(theta_ref,dp_ref,p_ref)
      // TODO: you could exploit exner computed in compute_theta_ref, to replace (p/p0)^(k-1) with exner*p0/p.
      auto theta_dp = [&](const int ilev)->Scalar {
        return theta(ilev)*dp(ilev);
      };
      m_eos.compute_phi_i(kv,m_geometry.m_phis(kv.ie,igp,jgp),
                             theta_dp,p,phi_i);

      // Replace vtheta_dp with theta (i.e., divide by dp)
      auto vtheta_dp = Homme::subview(m_state.m_vtheta_dp,kv.ie,m_data.np1,igp,jgp);
      auto state_dp = Homme::subview(m_state.m_dp3d,kv.ie,m_data.np1,igp,jgp);
      Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                           [&](const int ilev) {
        vtheta_dp[ilev] /= state_dp(ilev);
      });
    });
  }

  // first iter of laplace, const hv
  KOKKOS_INLINE_FUNCTION
  void operator() (const TagFirstLaplaceHV&, const TeamMember& team) const {
    KernelVariables kv(team);

    // Subtract the reference states from the states
    Kokkos::parallel_for(Kokkos::TeamThreadRange(kv.team,NP*NP),
                         [&](const int idx) {
      const int igp = idx / NP;
      const int jgp = idx % NP;

      auto theta = Homme::subview(m_state.m_vtheta_dp,kv.ie,m_data.np1,igp,jgp);
      auto phi_i = Homme::subview(m_state.m_phinh_i,kv.ie,m_data.np1,igp,jgp);
      auto dp    = Homme::subview(m_state.m_dp3d,kv.ie,m_data.np1,igp,jgp);

      auto theta_ref = Homme::subview(m_buffers.theta_ref,kv.ie,igp,jgp);
      auto phi_i_ref = Homme::subview(m_buffers.phi_i_ref,kv.ie,igp,jgp);
      auto dp_ref    = Homme::subview(m_buffers.dp_ref,kv.ie,igp,jgp);

      Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                           [&](const int ilev) {
        theta(ilev) -= theta_ref(ilev);
        phi_i(ilev) -= phi_i_ref(ilev);
        dp(ilev)    -= dp_ref(ilev);
      });
    });

    // Laplacian of layer thickness
    m_sphere_ops.laplace_simple(kv,
                   Homme::subview(m_state.m_dp3d,kv.ie,m_data.np1),
                   Homme::subview(m_buffers.dptens,kv.ie));
    // Laplacian of theta
    m_sphere_ops.laplace_simple(kv,
                   Homme::subview(m_state.m_vtheta_dp,kv.ie,m_data.np1),
                   Homme::subview(m_buffers.ttens,kv.ie));
    // Laplacian of vertical velocity (do not compute last interface)
#ifdef XX_NONBFB_COMING
    m_sphere_ops.laplace_simple<NUM_LEV_P,NUM_LEV_P,NUM_LEV>(kv,
#else
    m_sphere_ops.laplace_simple<NUM_LEV,NUM_LEV_P>(kv,
#endif
                   Homme::subview(m_state.m_w_i,kv.ie,m_data.np1),
                   Homme::subview(m_buffers.wtens,kv.ie));
    // Laplacian of geopotential (do not compute last interface)
    m_sphere_ops.laplace_simple<NUM_LEV,NUM_LEV_P>(kv,
                   Homme::subview(m_state.m_phinh_i,kv.ie,m_data.np1),
                   Homme::subview(m_buffers.phitens,kv.ie));
    // Laplacian of velocity
    m_sphere_ops.vlaplace_sphere_wk_contra(kv, m_data.nu_ratio1,
                              Homme::subview(m_state.m_v,kv.ie,m_data.np1),
                              Homme::subview(m_buffers.vtens,kv.ie));
  }


  //second iter of laplace, const hv
  KOKKOS_INLINE_FUNCTION
  void operator() (const TagSecondLaplaceConstHV&, const TeamMember& team) const {
    KernelVariables kv(team);
    // Laplacian of layers thickness
    m_sphere_ops.laplace_simple(kv,
                   Homme::subview(m_buffers.dptens,kv.ie),
                   Homme::subview(m_buffers.dptens,kv.ie));
    // Laplacian of theta
    m_sphere_ops.laplace_simple(kv,
                   Homme::subview(m_buffers.ttens,kv.ie),
                   Homme::subview(m_buffers.ttens,kv.ie));
    // Laplacian of vertical velocity
#ifdef XX_NONBFB_COMING
    m_sphere_ops.laplace_simple<NUM_LEV_P,NUM_LEV_P,NUM_LEV>(kv,
#else
    m_sphere_ops.laplace_simple(kv,
#endif
                   Homme::subview(m_buffers.wtens,kv.ie),
                   Homme::subview(m_buffers.wtens,kv.ie));
    // Laplacian of vertical geopotential
    m_sphere_ops.laplace_simple(kv,
                   Homme::subview(m_buffers.phitens,kv.ie),
                   Homme::subview(m_buffers.phitens,kv.ie));
    // Laplacian of velocity
    m_sphere_ops.vlaplace_sphere_wk_contra(kv, m_data.nu_ratio2,
                              Homme::subview(m_buffers.vtens,kv.ie),
                              Homme::subview(m_buffers.vtens,kv.ie));
  }

  //second iter of laplace, tensor hv
  KOKKOS_INLINE_FUNCTION
  void operator() (const TagSecondLaplaceTensorHV&, const TeamMember& team) const {
    KernelVariables kv(team);
    // Laplacian of layers thickness
    m_sphere_ops.laplace_tensor(kv,
                   Homme::subview(m_geometry.m_tensorvisc,kv.ie),
                   Homme::subview(m_buffers.dptens,kv.ie),
                   Homme::subview(m_buffers.dptens,kv.ie));
    // Laplacian of theta
    m_sphere_ops.laplace_tensor(kv,
                   Homme::subview(m_geometry.m_tensorvisc,kv.ie),
                   Homme::subview(m_buffers.ttens,kv.ie),
                   Homme::subview(m_buffers.ttens,kv.ie));
    // Laplacian of vertical velocity
#ifdef XX_NONBFB_COMING
    m_sphere_ops.laplace_simple<NUM_LEV_P,NUM_LEV_P,NUM_LEV>(kv,
#else
    m_sphere_ops.laplace_tensor(kv,
#endif
                   Homme::subview(m_geometry.m_tensorvisc,kv.ie),
                   Homme::subview(m_buffers.wtens,kv.ie),
                   Homme::subview(m_buffers.wtens,kv.ie));
    // Laplacian of geopotential
    m_sphere_ops.laplace_tensor(kv,
                   Homme::subview(m_geometry.m_tensorvisc,kv.ie),
                   Homme::subview(m_buffers.phitens,kv.ie),
                   Homme::subview(m_buffers.phitens,kv.ie));
    // Laplacian of velocity
    m_sphere_ops.vlaplace_sphere_wk_cartesian(kv, 
                   Homme::subview(m_geometry.m_tensorvisc,kv.ie),
                   Homme::subview(m_geometry.m_vec_sph2cart,kv.ie),
                   Homme::subview(m_buffers.vtens,kv.ie),
                   Homme::subview(m_buffers.vtens,kv.ie));
  }

  KOKKOS_INLINE_FUNCTION
  void operator() (const TagUpdateStates&, const TeamMember& team) const {
    KernelVariables kv(team);
    Kokkos::parallel_for(Kokkos::TeamThreadRange(kv.team,NP*NP),
                         [&](const int idx) {
      const int igp = idx / NP;
      const int jgp = idx % NP;

      // Add back the reference states for vtheta, phi and dp
      // Then, add Xtens quantities back to the states, except for vtheta
      auto u = Homme::subview(m_state.m_v,kv.ie,m_data.np1,0,igp,jgp);
      auto v = Homme::subview(m_state.m_v,kv.ie,m_data.np1,1,igp,jgp);
      auto w = Homme::subview(m_state.m_w_i,kv.ie,m_data.np1,igp,jgp);
      auto vtheta = Homme::subview(m_state.m_vtheta_dp,kv.ie,m_data.np1,igp,jgp);
      auto phi_i  = Homme::subview(m_state.m_phinh_i,kv.ie,m_data.np1,igp,jgp);
      auto dp     = Homme::subview(m_state.m_dp3d,kv.ie,m_data.np1,igp,jgp);

      auto vtheta_ref = Homme::subview(m_buffers.theta_ref,kv.ie,igp,jgp);
      auto phi_i_ref  = Homme::subview(m_buffers.phi_i_ref,kv.ie,igp,jgp);
      auto dp_ref     = Homme::subview(m_buffers.dp_ref,kv.ie,igp,jgp);

      auto utens   = Homme::subview(m_buffers.vtens,kv.ie,0,igp,jgp);
      auto vtens   = Homme::subview(m_buffers.vtens,kv.ie,1,igp,jgp);
      auto wtens   = Homme::subview(m_buffers.wtens,kv.ie,igp,jgp);
      auto ttens   = Homme::subview(m_buffers.ttens,kv.ie,igp,jgp);
      auto phitens = Homme::subview(m_buffers.phitens,kv.ie,igp,jgp);
      auto dptens  = Homme::subview(m_buffers.dptens,kv.ie,igp,jgp);
      Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                           [&](const int ilev) {
        vtheta(ilev) += vtheta_ref(ilev);
        phi_i(ilev)  += phi_i_ref(ilev);
        dp(ilev)     += dp_ref(ilev);

        u(ilev)     += utens(ilev);
        v(ilev)     += vtens(ilev);
        w(ilev)     += wtens(ilev);
        phi_i(ilev) += phitens(ilev);
        dp(ilev)    += dptens(ilev);
      });

      // Compute exner. Recycle old buffers for exner and p
      auto& exner = dp_ref;
      auto& p     = vtheta_ref;
      if (m_theta_hydrostatic_mode) {
        auto p_i = Homme::subview(m_buffers.p_i,kv.team_idx,igp,jgp);
        m_eos.compute_hydrostatic_p(kv,dp,p_i,p);
        m_eos.compute_exner(kv,p,exner);
      } else {
        auto vtheta_provider = [&](const int ilev)->Scalar {
          return vtheta(ilev)*dp(ilev);
        };
        m_eos.compute_pnh_and_exner(kv,vtheta_provider,phi_i,p,exner);
      }

      // Recycle vtheta_ref to store heating
      auto& heating = vtheta_ref;
      if (m_theta_hydrostatic_mode) {
        Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                             [&](const int ilev) {
          heating(ilev) = (u(ilev)*utens(ilev) + v(ilev)*vtens(ilev)) /
                           (exner(ilev)*PhysicalConstants::cp);
        });
      } else {
#ifdef XX_NONBFB_COMING
        using Info = ColInfo<NUM_PHYSICAL_LEV>;
        // Here, we fix f90 logic, and use k2=k+1, with the agreement that wtens(nlevp)=0.
        wtens(Info::LastPack)[Info::LastVecEnd] = 0.0;
        auto provider = [&w,&wtens](const int ilev)->Scalar {
          return w(ilev)*wtens(ilev);
        });
        m_col_ops.compute_midpoint_values<CombineMode::Add>(kv,provider,heating);
        Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                             [&](const int ilev) {
          heating(ilev) += u(ilev)*utens(ilev) + v(ilev)*vtens(ilev);
          heating(ilev) /= (exner(ilev)*PhysicalConstants::cp);
        });
#else
        using Info = ColInfo<NUM_INTERFACE_LEV>;
        // Here, we keep the original f90 logic, and exploit that k2=nlev.
        const Real last_lev_term = 
                w(Info::LastPack)[Info::LastVecEnd]*wtens(Info::LastPack)[Info::LastVecEnd];
        Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                             [&](const int ilev) {
          heating(ilev) += (w(ilev)*wtens(ilev) + last_lev_term)/2.0;
        });
#endif
      }

      // Add the heating term and convert theta back to vtheta
      Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team,NUM_LEV),
                           [&](const int ilev) {
        vtheta(ilev) += ttens(ilev)*m_hvcoord.dp0(ilev)*m_hvcoord.exner0(ilev) /
                      (exner(ilev)*dp(ilev));
        vtheta(ilev) -= heating(ilev);

        vtheta(ilev) *= dp(ilev);
      });

      // Adjust w_i at the surface, since velocity has changed
      Kokkos::single(Kokkos::PerThread(kv.team),[&](){
        using InfoI = ColInfo<NUM_INTERFACE_LEV>;
        using InfoM = ColInfo<NUM_PHYSICAL_LEV>;
        constexpr Real g = PhysicalConstants::g;
        const Real& grad_x = m_geometry.m_gradphis(kv.ie,0,igp,jgp);
        const Real& grad_y = m_geometry.m_gradphis(kv.ie,1,igp,jgp);
        w(InfoI::LastPack)[InfoI::LastVecEnd] = (u(InfoM::LastPack)[InfoM::LastVecEnd]*grad_x +
                                                 v(InfoM::LastPack)[InfoM::LastVecEnd]*grad_y) / g;
      });
    });
  }

  KOKKOS_INLINE_FUNCTION
  void operator()(const TagHyperPreExchange, const TeamMember &team) const {
    KernelVariables kv(team);
    Kokkos::parallel_for(Kokkos::TeamThreadRange(kv.team, NP * NP),
                         [&](const int &point_idx) {
      const int igp = point_idx / NP;
      const int jgp = point_idx % NP;

      const auto dpdiss_ave = Homme::subview(m_derived.m_dpdiss_ave,kv.ie, igp, jgp);
      const auto dpdiss_bih = Homme::subview(m_derived.m_dpdiss_biharmonic,kv.ie, igp, jgp);
      const auto dp3d = Homme::subview(m_state.m_dp3d,kv.ie, m_data.np1, igp, jgp);
      const auto dp_ref = Homme::subview(m_buffers.dp_ref,kv.ie, igp, jgp);
      const auto dptens = Homme::subview(m_buffers.dptens,kv.ie, igp, jgp);

      Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team, NUM_LEV),
                           [&](const int &ilev) {

        dpdiss_ave(ilev) += (m_data.eta_ave_w*dp3d(ilev) + dp_ref(ilev)) / m_data.hypervis_subcycle;
        dpdiss_bih(ilev) += m_data.eta_ave_w*dptens(ilev) / m_data.hypervis_subcycle;
      });
    });
    kv.team_barrier();

    // laplace subfunctors cannot be called from a TeamThreadRange or
    // ThreadVectorRange
    if (m_data.nu_top > 0) {

      //for top 3 levels and laplace, there is trivial nu_ratio only

      m_sphere_ops.laplace_simple<NUM_BIHARMONIC_LEV,NUM_LEV>(
            kv, Homme::subview(m_state.m_dp3d, kv.ie, m_data.np1),
                Homme::subview(m_buffers.lapl_dp, kv.team_idx));

      m_sphere_ops.laplace_simple<NUM_BIHARMONIC_LEV,NUM_LEV>(
            kv, Homme::subview(m_state.m_vtheta_dp, kv.ie, m_data.np1),
                Homme::subview(m_buffers.lapl_theta, kv.team_idx));

      m_sphere_ops.laplace_simple<NUM_BIHARMONIC_LEV,NUM_LEV_P>(
            kv, Homme::subview(m_state.m_w_i, kv.ie, m_data.np1),
                Homme::subview(m_buffers.lapl_w, kv.team_idx));

      m_sphere_ops.laplace_simple<NUM_BIHARMONIC_LEV,NUM_LEV_P>(
            kv, Homme::subview(m_state.m_phinh_i, kv.ie, m_data.np1),
                Homme::subview(m_buffers.lapl_phi, kv.team_idx));

      m_sphere_ops.vlaplace_sphere_wk_contra<NUM_BIHARMONIC_LEV,NUM_LEV>(
            kv, 1.0, Homme::subview(m_state.m_v, kv.ie, m_data.np1),
                     Homme::subview(m_buffers.lapl_v, kv.team_idx));
    }//if nu_top>0
    kv.team_barrier();

    Kokkos::parallel_for(Kokkos::TeamThreadRange(kv.team, NP * NP),
                         [&](const int &point_idx) {
      const int igp = point_idx / NP;
      const int jgp = point_idx % NP;
      Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team, NUM_LEV),
                           [&](const int &lev) {
        m_buffers.vtens(kv.ie, 0, igp, jgp, lev) *= -m_data.nu;
        m_buffers.vtens(kv.ie, 1, igp, jgp, lev) *= -m_data.nu;
        m_buffers.ttens(kv.ie, igp, jgp, lev) *= -m_data.nu;
        m_buffers.dptens(kv.ie, igp, jgp, lev) *= -m_data.nu_p;
        m_buffers.wtens(kv.ie, igp, jgp, lev) *= -m_data.nu;
        m_buffers.phitens(kv.ie, igp, jgp, lev) *= -m_data.nu_s;
      });

      if (m_data.nu_top > 0) {
        Kokkos::parallel_for(Kokkos::ThreadVectorRange(kv.team, int(NUM_BIHARMONIC_LEV)),
                           [&](const int ilev) {
          m_buffers.vtens(kv.ie, 0, igp, jgp, ilev) +=
              m_nu_scale_top[ilev] *
              m_buffers.lapl_v(kv.ie, 0, igp, jgp, ilev);
          m_buffers.vtens(kv.ie, 1, igp, jgp, ilev) +=
              m_nu_scale_top[ilev] *
              m_buffers.lapl_v(kv.ie, 1, igp, jgp, ilev);

          m_buffers.ttens(kv.ie, igp, jgp, ilev) +=
              m_nu_scale_top[ilev] *
              m_buffers.lapl_theta(kv.ie, igp, jgp, ilev);

          m_buffers.dptens(kv.ie, igp, jgp, ilev) +=
              m_nu_scale_top[ilev] *
              m_buffers.lapl_dp(kv.ie, igp, jgp, ilev);

          m_buffers.wtens(kv.ie, igp, jgp, ilev) +=
              m_nu_scale_top[ilev] *
              m_buffers.lapl_w(kv.ie, igp, jgp, ilev);
          m_buffers.phitens(kv.ie, igp, jgp, ilev) +=
              m_nu_scale_top[ilev] *
              m_buffers.lapl_phi(kv.ie, igp, jgp, ilev);
        });
      }
    });
  }

protected:

  HyperviscosityData    m_data;
  ElementsState         m_state;
  ElementsDerivedState  m_derived;
  ElementsGeometry      m_geometry;
  SphereOperators       m_sphere_ops;
  ColumnOps             m_col_ops;
  ElementOps            m_elem_ops;
  EquationOfState       m_eos;
  Buffers               m_buffers;
  HybridVCoord          m_hvcoord;

  bool m_theta_hydrostatic_mode;

  // Policies
  Kokkos::TeamPolicy<ExecSpace,TagRefStates>        m_policy_ref_states;
  Kokkos::TeamPolicy<ExecSpace,TagUpdateStates>     m_policy_update_states;
  Kokkos::TeamPolicy<ExecSpace,TagFirstLaplaceHV>   m_policy_first_laplace;
  Kokkos::TeamPolicy<ExecSpace,TagHyperPreExchange> m_policy_pre_exchange;

  std::shared_ptr<BoundaryExchange> m_be;

  ExecViewManaged<Scalar[NUM_LEV]> m_nu_scale_top;
};

} // namespace Homme

#endif // HOMMEXX_HYPERVISCOSITY_FUNCTOR_IMPL_HPP