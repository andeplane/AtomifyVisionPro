// clang-format off
/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   aE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Yury Lysogorskiy (ICAMS)
------------------------------------------------------------------------- */

#include "pair_pace_extrapolation_kokkos.h"

#include "atom_kokkos.h"
#include "atom_masks.h"
#include "comm.h"
#include "error.h"
#include "force.h"
#include "kokkos.h"
#include "math_const.h"
#include "memory_kokkos.h"
#include "neighbor_kokkos.h"
#include "neigh_request.h"

#include "ace-evaluator/ace_version.h"
#include "ace-evaluator/ace_radial.h"

#include "ace/ace_b_basis.h"
#include "ace/ace_b_evaluator.h"

#include <cstring>

namespace LAMMPS_NS {
  struct ACEALImpl {
    ACEALImpl() : basis_set(nullptr), ace(nullptr) {}

    ~ACEALImpl() {
      delete basis_set;
      delete ace;
    }

    ACEBBasisSet *basis_set;
    ACEBEvaluator *ace;
  };
} // namespace LAMMPS_NS

using namespace LAMMPS_NS;
using namespace MathConst;

enum{FS,FS_SHIFTEDSCALED};

/* ---------------------------------------------------------------------- */

template<class DeviceType>
PairPACEExtrapolationKokkos<DeviceType>::PairPACEExtrapolationKokkos(LAMMPS *lmp) : PairPACEExtrapolation(lmp)
{
  respa_enable = 0;

  kokkosable = 1;
  atomKK = (AtomKokkos *) atom;
  execution_space = ExecutionSpaceFromDevice<DeviceType>::space;
  datamask_read = EMPTY_MASK;
  datamask_modify = EMPTY_MASK;

  host_flag = (execution_space == Host);
}

/* ----------------------------------------------------------------------
   check if allocated, since class can be destructed when incomplete
------------------------------------------------------------------------- */

template<class DeviceType>
PairPACEExtrapolationKokkos<DeviceType>::~PairPACEExtrapolationKokkos()
{
  if (copymode) return;

  memoryKK->destroy_kokkos(k_eatom,eatom);
  memoryKK->destroy_kokkos(k_vatom,vatom);

  // deallocate views of views in serial to prevent issues in Kokkos tools

  if (k_splines_gk.h_view.data()) {
    for (int i = 0; i < nelements; i++) {
      for (int j = 0; j < nelements; j++) {
        k_splines_gk.h_view(i, j).deallocate();
        k_splines_rnl.h_view(i, j).deallocate();
        k_splines_hc.h_view(i, j).deallocate();
      }
    }
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::grow(int natom, int maxneigh)
{
  auto basis_set = aceimpl->basis_set;

  if ((int)A.extent(0) < natom) {

    MemKK::realloc_kokkos(A_sph, "pace:A_sph", natom, nelements, idx_sph_max, nradmax + 1);
    MemKK::realloc_kokkos(A, "pace:A", natom, nelements, (lmax + 1) * (lmax + 1), nradmax + 1);
    MemKK::realloc_kokkos(A_rank1, "pace:A_rank1", natom, nelements, nradbase);

    MemKK::realloc_kokkos(A_list, "pace:A_list", natom, idx_ms_combs_max, basis_set->rankmax);
    //size is +1 of max to avoid out-of-boundary array access in double-triangular scheme
    MemKK::realloc_kokkos(A_forward_prod, "pace:A_forward_prod", natom, idx_ms_combs_max, basis_set->rankmax + 1);

    MemKK::realloc_kokkos(e_atom, "pace:e_atom", natom);
    MemKK::realloc_kokkos(rhos, "pace:rhos", natom, basis_set->ndensitymax + 1); // +1 density for core repulsion
    MemKK::realloc_kokkos(dF_drho, "pace:dF_drho", natom, basis_set->ndensitymax + 1); // +1 density for core repulsion

    MemKK::realloc_kokkos(weights, "pace:weights", natom, nelements, idx_sph_max, nradmax + 1);
    MemKK::realloc_kokkos(weights_rank1, "pace:weights_rank1", natom, nelements, nradbase);

    // hard-core repulsion
    MemKK::realloc_kokkos(rho_core, "pace:rho_core", natom);
    MemKK::realloc_kokkos(dF_drho_core, "pace:dF_drho_core", natom);
    MemKK::realloc_kokkos(dF_dfcut, "pace:dF_dfcut", natom);
    MemKK::realloc_kokkos(d_d_min, "pace:r_min_pair", natom);
    MemKK::realloc_kokkos(d_jj_min, "pace:j_min_pair", natom);
    MemKK::realloc_kokkos(d_corerep, "pace:corerep", natom); // per-atom corerep

    MemKK::realloc_kokkos(dB_flatten, "pace:dB_flatten", natom, idx_ms_combs_max, basis_set->rankmax);

    // B-projections
    MemKK::realloc_kokkos(projections, "pace:projections", natom, total_num_functions_max); // per-atom B-projections
    MemKK::realloc_kokkos(d_gamma, "pace:gamma", natom); // per-atom gamma
  }

  if (((int)fr.extent(0) < natom) || ((int)fr.extent(1) < maxneigh)) {

    // radial functions
    MemKK::realloc_kokkos(fr, "pace:fr", natom, maxneigh, lmax + 1, nradmax);
    MemKK::realloc_kokkos(dfr, "pace:dfr", natom, maxneigh, lmax + 1, nradmax);
    MemKK::realloc_kokkos(gr, "pace:gr", natom, maxneigh, nradbase);
    MemKK::realloc_kokkos(dgr, "pace:dgr", natom, maxneigh, nradbase);
    const int max_num_functions = MAX(nradbase, nradmax*(lmax + 1));
    MemKK::realloc_kokkos(d_values, "pace:d_values", natom, maxneigh, max_num_functions);
    MemKK::realloc_kokkos(d_derivatives, "pace:d_derivatives", natom, maxneigh, max_num_functions);

    // hard-core repulsion
    MemKK::realloc_kokkos(cr, "pace:cr", natom, maxneigh);
    MemKK::realloc_kokkos(dcr, "pace:dcr", natom, maxneigh);

    // short neigh list
    MemKK::realloc_kokkos(d_ncount, "pace:ncount", natom);
    MemKK::realloc_kokkos(d_mu, "pace:mu", natom, maxneigh);
    MemKK::realloc_kokkos(d_rhats, "pace:rhats", natom, maxneigh);
    MemKK::realloc_kokkos(d_rnorms, "pace:rnorms", natom, maxneigh);
    MemKK::realloc_kokkos(d_nearest, "pace:nearest", natom, maxneigh);

    MemKK::realloc_kokkos(f_ij, "pace:f_ij", natom, maxneigh);
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::copy_pertype()
{
  auto basis_set = aceimpl->basis_set;

  MemKK::realloc_kokkos(d_rho_core_cutoff, "pace:rho_core_cutoff", nelements);
  MemKK::realloc_kokkos(d_drho_core_cutoff, "pace:drho_core_cutoff", nelements);
  MemKK::realloc_kokkos(d_E0vals, "pace:E0vals", nelements);
  MemKK::realloc_kokkos(d_ndensity, "pace:ndensity", nelements);
  MemKK::realloc_kokkos(d_npoti, "pace:npoti", nelements);

  auto h_rho_core_cutoff = Kokkos::create_mirror_view(d_rho_core_cutoff);
  auto h_drho_core_cutoff = Kokkos::create_mirror_view(d_drho_core_cutoff);
  auto h_E0vals = Kokkos::create_mirror_view(d_E0vals);
  auto h_ndensity = Kokkos::create_mirror_view(d_ndensity);
  auto h_npoti = Kokkos::create_mirror_view(d_npoti);

  for (int n = 0; n < nelements; n++) {
    h_rho_core_cutoff[n] = basis_set->map_embedding_specifications.at(n).rho_core_cutoff;
    h_drho_core_cutoff[n] = basis_set->map_embedding_specifications.at(n).drho_core_cutoff;

    h_E0vals(n) = basis_set->E0vals(n);

    h_ndensity(n) = basis_set->map_embedding_specifications.at(n).ndensity;

    string npoti = basis_set->map_embedding_specifications.at(n).npoti;
    if (npoti == "FinnisSinclair")
      h_npoti(n) = FS;
    else if (npoti == "FinnisSinclairShiftedScaled")
      h_npoti(n) = FS_SHIFTEDSCALED;
  }

  Kokkos::deep_copy(d_rho_core_cutoff, h_rho_core_cutoff);
  Kokkos::deep_copy(d_drho_core_cutoff, h_drho_core_cutoff);
  Kokkos::deep_copy(d_E0vals, h_E0vals);
  Kokkos::deep_copy(d_ndensity, h_ndensity);
  Kokkos::deep_copy(d_npoti, h_npoti);

  MemKK::realloc_kokkos(d_wpre, "pace:wpre", nelements, basis_set->ndensitymax);
  MemKK::realloc_kokkos(d_mexp, "pace:mexp", nelements, basis_set->ndensitymax);

  auto h_wpre = Kokkos::create_mirror_view(d_wpre);
  auto h_mexp = Kokkos::create_mirror_view(d_mexp);

  for (int n = 0; n < nelements; n++) {
    const int ndensity = basis_set->map_embedding_specifications.at(n).ndensity;
    for (int p = 0; p < ndensity; p++) {
      h_wpre(n, p) = basis_set->map_embedding_specifications.at(n).FS_parameters.at(p * 2 + 0);
      h_mexp(n, p) = basis_set->map_embedding_specifications.at(n).FS_parameters.at(p * 2 + 1);
    }
  }

  Kokkos::deep_copy(d_wpre, h_wpre);
  Kokkos::deep_copy(d_mexp, h_mexp);

  // ZBL core-rep
  MemKK::realloc_kokkos(d_cut_in, "pace:d_cut_in", nelements, nelements);
  MemKK::realloc_kokkos(d_dcut_in, "pace:d_dcut_in", nelements, nelements);
  auto h_cut_in = Kokkos::create_mirror_view(d_cut_in);
  auto h_dcut_in = Kokkos::create_mirror_view(d_dcut_in);

  for (int mu_i = 0; mu_i < nelements; ++mu_i) {
    for (int mu_j = 0; mu_j < nelements; ++mu_j) {
      h_cut_in(mu_i,mu_j) = basis_set->map_bond_specifications.at({mu_i,mu_j}).rcut_in;
      h_dcut_in(mu_i,mu_j) = basis_set->map_bond_specifications.at({mu_i,mu_j}).dcut_in;
    }
  }
  Kokkos::deep_copy(d_cut_in, h_cut_in);
  Kokkos::deep_copy(d_dcut_in, h_dcut_in);

  is_zbl = basis_set->radial_functions->inner_cutoff_type == "zbl";
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::copy_splines()
{
  auto basis_set = aceimpl->basis_set;

  if (k_splines_gk.d_view.data()) {
    for (int i = 0; i < nelements; i++) {
      for (int j = 0; j < nelements; j++) {
        k_splines_gk.h_view(i, j).deallocate();
        k_splines_rnl.h_view(i, j).deallocate();
        k_splines_hc.h_view(i, j).deallocate();
      }
    }
  }

  k_splines_gk = Kokkos::DualView<SplineInterpolatorKokkos**, DeviceType>("pace:splines_gk", nelements, nelements);
  k_splines_rnl = Kokkos::DualView<SplineInterpolatorKokkos**, DeviceType>("pace:splines_rnl", nelements, nelements);
  k_splines_hc = Kokkos::DualView<SplineInterpolatorKokkos**, DeviceType>("pace:splines_hc", nelements, nelements);

  ACERadialFunctions* radial_functions = dynamic_cast<ACERadialFunctions*>(basis_set->radial_functions);

  if (radial_functions == nullptr)
    error->all(FLERR,"Chosen radial basis style not supported by pair style pace/kk");

  for (int i = 0; i < nelements; i++) {
    for (int j = 0; j < nelements; j++) {
      k_splines_gk.h_view(i, j) = radial_functions->splines_gk(i, j);
      k_splines_rnl.h_view(i, j) = radial_functions->splines_rnl(i, j);
      k_splines_hc.h_view(i, j) = radial_functions->splines_hc(i, j);
    }
  }

  k_splines_gk.modify_host();
  k_splines_rnl.modify_host();
  k_splines_hc.modify_host();

  k_splines_gk.sync_device();
  k_splines_rnl.sync_device();
  k_splines_hc.sync_device();
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::copy_tilde()
{
  auto basis_set = aceimpl->basis_set;
  auto b_evaluator = aceimpl->ace;

  // flatten loops, get per-element count and max

  idx_ms_combs_max = 0;
  total_num_functions_max = 0;

  MemKK::realloc_kokkos(d_idx_ms_combs_count, "pace:idx_ms_combs_count", nelements);
  auto h_idx_ms_combs_count = Kokkos::create_mirror_view(d_idx_ms_combs_count);

  MemKK::realloc_kokkos(d_total_basis_size, "pace:total_basis_size", nelements);
  auto h_total_basis_size = Kokkos::create_mirror_view(d_total_basis_size);

  for (int mu = 0; mu < nelements; mu++) {
    int idx_ms_combs = 0;
    const int total_basis_size_rank1 = basis_set->total_basis_size_rank1[mu];
    const int total_basis_size = basis_set->total_basis_size[mu];

    ACEBBasisFunction *basis = basis_set->basis[mu];

    // rank=1
    for (int func_rank1_ind = 0; func_rank1_ind < total_basis_size_rank1; ++func_rank1_ind)
      idx_ms_combs++;

    // rank > 1
    for (int idx_func = 0; idx_func < total_basis_size; ++idx_func) {
      ACEBBasisFunction *func = &basis[idx_func];

      // loop over {ms} combinations in sum
      for (int ms_ind = 0; ms_ind < func->num_ms_combs; ++ms_ind)
        idx_ms_combs++;
    }
    h_idx_ms_combs_count(mu) = idx_ms_combs;
    idx_ms_combs_max = MAX(idx_ms_combs_max, idx_ms_combs);
    total_num_functions_max = MAX(total_num_functions_max, total_basis_size_rank1 + total_basis_size);
    h_total_basis_size(mu) = total_basis_size_rank1 + total_basis_size;
  }

  Kokkos::deep_copy(d_idx_ms_combs_count, h_idx_ms_combs_count);
  Kokkos::deep_copy(d_total_basis_size, h_total_basis_size);

  MemKK::realloc_kokkos(d_rank, "pace:rank", nelements, total_num_functions_max);
  MemKK::realloc_kokkos(d_num_ms_combs, "pace:num_ms_combs", nelements, total_num_functions_max);
  MemKK::realloc_kokkos(d_idx_funcs, "pace:idx_funcs", nelements, idx_ms_combs_max);
  MemKK::realloc_kokkos(d_mus, "pace:mus", nelements, total_num_functions_max, basis_set->rankmax);
  MemKK::realloc_kokkos(d_ns, "pace:ns", nelements, total_num_functions_max, basis_set->rankmax);
  MemKK::realloc_kokkos(d_ls, "pace:ls", nelements, total_num_functions_max, basis_set->rankmax);
  MemKK::realloc_kokkos(d_ms_combs, "pace:ms_combs", nelements, idx_ms_combs_max, basis_set->rankmax);
  MemKK::realloc_kokkos(d_gen_cgs, "pace:gen_cgs", nelements, idx_ms_combs_max);
  MemKK::realloc_kokkos(d_coeffs, "pace:coeffs", nelements, total_num_functions_max, basis_set->ndensitymax);
  // active set inverted
  t_ace_3d d_ASI_temp;
  MemKK::realloc_kokkos(d_ASI_temp, "pace:ASI_temp", nelements, total_num_functions_max, total_num_functions_max);

  auto h_rank = Kokkos::create_mirror_view(d_rank);
  auto h_num_ms_combs = Kokkos::create_mirror_view(d_num_ms_combs);
  auto h_idx_funcs = Kokkos::create_mirror_view(d_idx_funcs);
  auto h_mus = Kokkos::create_mirror_view(d_mus);
  auto h_ns = Kokkos::create_mirror_view(d_ns);
  auto h_ls = Kokkos::create_mirror_view(d_ls);
  auto h_ms_combs = Kokkos::create_mirror_view(d_ms_combs);
  auto h_gen_cgs = Kokkos::create_mirror_view(d_gen_cgs);
  auto h_coeffs = Kokkos::create_mirror_view(d_coeffs);
  // asi
  auto h_ASI = Kokkos::create_mirror_view(d_ASI_temp);

  // copy values on host

  for (int mu = 0; mu < nelements; mu++) {
    const int total_basis_size_rank1 = basis_set->total_basis_size_rank1[mu];
    const int total_basis_size = basis_set->total_basis_size[mu];

    ACEBBasisFunction *basis_rank1 = basis_set->basis_rank1[mu];
    ACEBBasisFunction *basis = basis_set->basis[mu];

    const int ndensity = basis_set->map_embedding_specifications.at(mu).ndensity;

    int idx_ms_combs = 0;

    // rank=1
    for (int idx_func = 0; idx_func < total_basis_size_rank1; ++idx_func) {
      ACEBBasisFunction *func = &basis_rank1[idx_func];
      h_rank(mu, idx_func) = 1;
      h_mus(mu, idx_func, 0) = func->mus[0];
      h_ns(mu, idx_func, 0) = func->ns[0];

      for (int p = 0; p < ndensity; ++p)
        h_coeffs(mu, idx_func, p) = func->coeff[p];

      h_gen_cgs(mu, idx_ms_combs) = func->gen_cgs[0];

      h_idx_funcs(mu, idx_ms_combs) = idx_func;
      idx_ms_combs++;
    }

    // rank > 1
    for (int idx_func = 0; idx_func < total_basis_size; ++idx_func) {
      ACEBBasisFunction *func = &basis[idx_func];
      // TODO: check if func->ctildes are zero, then skip

      const int idx_func_through = total_basis_size_rank1 + idx_func;

      const int rank = h_rank(mu, idx_func_through) = func->rank;
      h_num_ms_combs(mu, idx_func_through) = func->num_ms_combs;
      for (int t = 0; t < rank; t++) {
        h_mus(mu, idx_func_through, t) = func->mus[t];
        h_ns(mu, idx_func_through, t) = func->ns[t];
        h_ls(mu, idx_func_through, t) = func->ls[t];
      }

      for (int p = 0; p < ndensity; ++p)
        h_coeffs(mu, idx_func_through, p) = func->coeff[p];

      // loop over {ms} combinations in sum
      for (int ms_ind = 0; ms_ind < func->num_ms_combs; ++ms_ind) {
        auto ms = &func->ms_combs[ms_ind * rank]; // current ms-combination (of length = rank)
        for (int t = 0; t < rank; t++)
          h_ms_combs(mu, idx_ms_combs, t) = ms[t];

        h_gen_cgs(mu, idx_ms_combs) = func->gen_cgs[ms_ind];

        h_idx_funcs(mu, idx_ms_combs) = idx_func_through;
        idx_ms_combs++;
      }
    }

    // ASI
    const auto &A_as_inv = b_evaluator->A_active_set_inv.at(mu);
    for (int i = 0; i < total_basis_size_rank1 + total_basis_size; i++)
      for (int j = 0; j < total_basis_size_rank1 + total_basis_size; j++){
        h_ASI(mu,i,j) = A_as_inv(j,i); // transpose back for better performance on GPU
    }
  }

  Kokkos::deep_copy(d_rank, h_rank);
  Kokkos::deep_copy(d_num_ms_combs, h_num_ms_combs);
  Kokkos::deep_copy(d_idx_funcs, h_idx_funcs);
  Kokkos::deep_copy(d_mus, h_mus);
  Kokkos::deep_copy(d_ns, h_ns);
  Kokkos::deep_copy(d_ls, h_ls);
  Kokkos::deep_copy(d_ms_combs, h_ms_combs);
  Kokkos::deep_copy(d_gen_cgs, h_gen_cgs);
  Kokkos::deep_copy(d_coeffs, h_coeffs);
  Kokkos::deep_copy(d_ASI_temp, h_ASI);
  d_ASI = d_ASI_temp; // copy from temopary array to const array
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::init_style()
{
  if (host_flag) {
    if (lmp->kokkos->nthreads > 1)
      error->all(FLERR,"Pair style pace/extrapolation/kk can currently only run on a single "
                         "CPU thread");

    PairPACEExtrapolation::init_style();
    return;
  }

  if (atom->tag_enable == 0) error->all(FLERR, "Pair style PACE requires atom IDs");
  if (force->newton_pair == 0) error->all(FLERR, "Pair style PACE requires newton pair on");

  // neighbor list request for KOKKOS

  neighflag = lmp->kokkos->neighflag;

  auto request = neighbor->add_request(this, NeighConst::REQ_FULL);
  request->set_kokkos_host(std::is_same_v<DeviceType,LMPHostType> &&
                           !std::is_same_v<DeviceType,LMPDeviceType>);
  request->set_kokkos_device(std::is_same_v<DeviceType,LMPDeviceType>);
  if (neighflag == FULL)
    error->all(FLERR,"Must use half neighbor list style with pair pace/kk");

  auto basis_set = aceimpl->basis_set;

  nelements = basis_set->nelements;
  lmax = basis_set->lmax;
  nradmax = basis_set->nradmax;
  nradbase = basis_set->nradbase;

  // spherical harmonics

  MemKK::realloc_kokkos(d_idx_sph, "pace:idx_sph", (lmax + 1) * (lmax + 1));
  MemKK::realloc_kokkos(alm, "pace:alm", (lmax + 1) * (lmax + 1));
  MemKK::realloc_kokkos(blm, "pace:blm", (lmax + 1) * (lmax + 1));
  MemKK::realloc_kokkos(cl, "pace:cl", lmax + 1);
  MemKK::realloc_kokkos(dl, "pace:dl", lmax + 1);

  pre_compute_harmonics(lmax);
  copy_pertype();
  copy_splines();
  copy_tilde();
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

template<class DeviceType>
double PairPACEExtrapolationKokkos<DeviceType>::init_one(int i, int j)
{
  double cutone = PairPACEExtrapolation::init_one(i,j);

  k_scale.h_view(i,j) = k_scale.h_view(j,i) = scale[i][j];
  k_scale.template modify<LMPHostType>();

  k_cutsq.h_view(i,j) = k_cutsq.h_view(j,i) = cutone*cutone;
  k_cutsq.template modify<LMPHostType>();

  return cutone;
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::coeff(int narg, char **arg)
{
  PairPACEExtrapolation::coeff(narg,arg);

  auto b_evaluator = aceimpl->ace;
  if (!b_evaluator->get_is_linear_extrapolation_grade()) {
        error->all(FLERR,"Must use LINEAR ASI with pair pace/extrapolation/kk");
  }
  // Set up element lists

  auto h_map = Kokkos::create_mirror_view(d_map);

  for (int i = 1; i <= atom->ntypes; i++)
    h_map(i) = map[i];

  Kokkos::deep_copy(d_map,h_map);
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::allocate()
{
  PairPACEExtrapolation::allocate();

  int n = atom->ntypes + 1;
  MemKK::realloc_kokkos(d_map, "pace:map", n);

  MemKK::realloc_kokkos(k_cutsq, "pace:cutsq", n, n);
  d_cutsq = k_cutsq.template view<DeviceType>();

  MemKK::realloc_kokkos(k_scale, "pace:scale", n, n);
  d_scale = k_scale.template view<DeviceType>();
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
struct FindMaxNumNeighs {
  typedef DeviceType device_type;
  NeighListKokkos<DeviceType> k_list;

  FindMaxNumNeighs(NeighListKokkos<DeviceType>* nl): k_list(*nl) {}
  ~FindMaxNumNeighs() {k_list.copymode = 1;}

  KOKKOS_INLINE_FUNCTION
  void operator() (const int& ii, int& maxneigh) const {
    const int i = k_list.d_ilist[ii];
    const int num_neighs = k_list.d_numneigh[i];
    if (maxneigh < num_neighs) maxneigh = num_neighs;
  }
};

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::compute(int eflag_in, int vflag_in)
{
  if (host_flag) {
    atomKK->sync(Host,X_MASK|TYPE_MASK);
    PairPACEExtrapolation::compute(eflag_in,vflag_in);
    atomKK->modified(Host,F_MASK);
    return;
  }

  eflag = eflag_in;
  vflag = vflag_in;

  if (neighflag == FULL) no_virial_fdotr_compute = 1;

  ev_init(eflag,vflag,0);

  // reallocate per-atom arrays if necessary

  if (eflag_atom) {
    memoryKK->destroy_kokkos(k_eatom,eatom);
    memoryKK->create_kokkos(k_eatom,eatom,maxeatom,"pair:eatom");
    d_eatom = k_eatom.view<DeviceType>();
  }
  if (vflag_atom) {
    memoryKK->destroy_kokkos(k_vatom,vatom);
    memoryKK->create_kokkos(k_vatom,vatom,maxvatom,"pair:vatom");
    d_vatom = k_vatom.view<DeviceType>();
  }

  if (flag_compute_extrapolation_grade && atom->nlocal > nmax) {
        memory->destroy(extrapolation_grade_gamma);
        nmax = atom->nlocal;
        memory->create(extrapolation_grade_gamma, nmax, "pace/atom:gamma");
        //zeroify array
        memset(extrapolation_grade_gamma, 0, nmax * sizeof(*extrapolation_grade_gamma));
  }

  if (flag_corerep_factor && atom->nlocal > nmax_corerep) {
    memory->destroy(corerep_factor);
    nmax_corerep = atom->nlocal;
    memory->create(corerep_factor, nmax_corerep, "pace/atom:corerep");
    //zeroify array
    memset(corerep_factor, 0, nmax_corerep * sizeof(*corerep_factor));
  }

  copymode = 1;
  if (!force->newton_pair)
    error->all(FLERR,"PairPACEExtrapolationKokkos requires 'newton on'");

  atomKK->sync(execution_space,X_MASK|F_MASK|TYPE_MASK);
  x = atomKK->k_x.view<DeviceType>();
  f = atomKK->k_f.view<DeviceType>();
  type = atomKK->k_type.view<DeviceType>();
  k_scale.template sync<DeviceType>();
  k_cutsq.template sync<DeviceType>();

  NeighListKokkos<DeviceType>* k_list = static_cast<NeighListKokkos<DeviceType>*>(list);
  d_numneigh = k_list->d_numneigh;
  d_neighbors = k_list->d_neighbors;
  d_ilist = k_list->d_ilist;
  inum = list->inum;

  need_dup = lmp->kokkos->need_dup<DeviceType>();
  if (need_dup) {
    dup_f     = Kokkos::Experimental::create_scatter_view<Kokkos::Experimental::ScatterSum, Kokkos::Experimental::ScatterDuplicated>(f);
    dup_vatom = Kokkos::Experimental::create_scatter_view<Kokkos::Experimental::ScatterSum, Kokkos::Experimental::ScatterDuplicated>(d_vatom);
  } else {
    ndup_f     = Kokkos::Experimental::create_scatter_view<Kokkos::Experimental::ScatterSum, Kokkos::Experimental::ScatterNonDuplicated>(f);
    ndup_vatom = Kokkos::Experimental::create_scatter_view<Kokkos::Experimental::ScatterSum, Kokkos::Experimental::ScatterNonDuplicated>(d_vatom);
  }

  maxneigh = 0;
  Kokkos::parallel_reduce("pace::find_maxneigh", inum, FindMaxNumNeighs<DeviceType>(k_list), Kokkos::Max<int>(maxneigh));

  int vector_length_default = 1;
  int team_size_default = 1;
  if (!host_flag)
    team_size_default = 32;

  chunk_size = MIN(chunksize,inum); // "chunksize" variable is set by user
  chunk_offset = 0;

  grow(chunk_size, maxneigh);

  EV_FLOAT ev;

  while (chunk_offset < inum) { // chunk up loop to prevent running out of memory

    Kokkos::deep_copy(weights, 0.0);
    Kokkos::deep_copy(weights_rank1, 0.0);
    Kokkos::deep_copy(A_sph, 0.0);
    Kokkos::deep_copy(A_rank1, 0.0);
    Kokkos::deep_copy(rhos, 0.0);
    Kokkos::deep_copy(rho_core, 0.0);
    Kokkos::deep_copy(d_d_min, PairPACEExtrapolation::aceimpl->basis_set->cutoffmax);
    Kokkos::deep_copy(d_jj_min, -1);
    Kokkos::deep_copy(projections, 0.0);
    Kokkos::deep_copy(d_gamma, 0.0);
    Kokkos::deep_copy(d_corerep, 0.0);

    EV_FLOAT ev_tmp;

    if (chunk_size > inum - chunk_offset)
      chunk_size = inum - chunk_offset;

    //Neigh
    {
      int vector_length = vector_length_default;
      int team_size = team_size_default;
      check_team_size_for<TagPairPACEComputeNeigh>(chunk_size,team_size,vector_length);
      int scratch_size = scratch_size_helper<int>(team_size * maxneigh);
      typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeNeigh> policy_neigh(chunk_size,team_size,vector_length);
      policy_neigh = policy_neigh.set_scratch_size(0, Kokkos::PerTeam(scratch_size));
      Kokkos::parallel_for("ComputeNeigh",policy_neigh,*this);
    }

    //ComputeRadial
    {
      int vector_length = vector_length_default;
      int team_size = team_size_default;
      check_team_size_for<TagPairPACEComputeRadial>(((chunk_size+team_size-1)/team_size)*maxneigh,team_size,vector_length);
      typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeRadial> policy_radial(((chunk_size+team_size-1)/team_size)*maxneigh,team_size,vector_length);
      Kokkos::parallel_for("ComputeRadial",policy_radial,*this);
    }

    //ComputeAi
    {
      int vector_length = vector_length_default;
      int team_size = team_size_default;
      check_team_size_for<TagPairPACEComputeAi>(((chunk_size+team_size-1)/team_size)*maxneigh,team_size,vector_length);
      typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeAi> policy_ai(((chunk_size+team_size-1)/team_size)*maxneigh,team_size,vector_length);
      Kokkos::parallel_for("ComputeAi",policy_ai,*this);
    }

    //ConjugateAi
    {
      typename Kokkos::RangePolicy<DeviceType,TagPairPACEConjugateAi> policy_conj_ai(0,chunk_size);
      Kokkos::parallel_for("ConjugateAi",policy_conj_ai,*this);
    }

    //ComputeRho
    {
      typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeRho> policy_rho(0, chunk_size * idx_ms_combs_max);
      Kokkos::parallel_for("ComputeRho",policy_rho,*this);
    }

    //ComputeFS
    {
      typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeFS> policy_fs(0,chunk_size);
      Kokkos::parallel_for("ComputeFS",policy_fs,*this);
    }

    //ComputeGamma
    if (flag_compute_extrapolation_grade) {
      typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeGamma> policy_gamma(0,chunk_size);
      Kokkos::parallel_for("ComputeGamma",policy_gamma,*this);
    }

    //ComputeWeights
    {
      typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeWeights> policy_weights(0,chunk_size * idx_ms_combs_max);
      Kokkos::parallel_for("ComputeWeights",policy_weights,*this);
    }

    //ComputeDerivative
    {
      int vector_length = vector_length_default;
      int team_size = team_size_default;
      check_team_size_for<TagPairPACEComputeDerivative>(((chunk_size+team_size-1)/team_size)*maxneigh,team_size,vector_length);
      typename Kokkos::TeamPolicy<DeviceType,TagPairPACEComputeDerivative> policy_derivative(((chunk_size+team_size-1)/team_size)*maxneigh,team_size,vector_length);
      Kokkos::parallel_for("ComputeDerivative",policy_derivative,*this);
    }

    //ComputeForce
    {
      if (evflag) {
        if (neighflag == HALF) {
          typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeForce<HALF,1> > policy_force(0,chunk_size);
          Kokkos::parallel_reduce(policy_force, *this, ev_tmp);
        } else if (neighflag == HALFTHREAD) {
          typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeForce<HALFTHREAD,1> > policy_force(0,chunk_size);
          Kokkos::parallel_reduce("ComputeForce",policy_force, *this, ev_tmp);
        }
      } else {
        if (neighflag == HALF) {
          typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeForce<HALF,0> > policy_force(0,chunk_size);
          Kokkos::parallel_for(policy_force, *this);
        } else if (neighflag == HALFTHREAD) {
          typename Kokkos::RangePolicy<DeviceType,TagPairPACEComputeForce<HALFTHREAD,0> > policy_force(0,chunk_size);
          Kokkos::parallel_for("ComputeForce",policy_force, *this);
        }
      }
    }
    ev += ev_tmp;

    // if flag_compute_extrapolation_grade - copy current d_gamma to extrapolation_grade_gamma

    if (flag_compute_extrapolation_grade){
        h_gamma = Kokkos::create_mirror_view(d_gamma);
        Kokkos::deep_copy(h_gamma, d_gamma);
        memcpy(extrapolation_grade_gamma+chunk_offset, (void *) h_gamma.data(), sizeof(double)*chunk_size);
    }

    if (flag_corerep_factor) {
      h_corerep = Kokkos::create_mirror_view(d_corerep);
      Kokkos::deep_copy(h_corerep,d_corerep);
      memcpy(corerep_factor+chunk_offset, (void *) h_corerep.data(), sizeof(double)*chunk_size);
    }

    chunk_offset += chunk_size;
  } // end while

  if (need_dup)
    Kokkos::Experimental::contribute(f, dup_f);

  if (eflag_global) eng_vdwl += ev.evdwl;
  if (vflag_global) {
    virial[0] += ev.v[0];
    virial[1] += ev.v[1];
    virial[2] += ev.v[2];
    virial[3] += ev.v[3];
    virial[4] += ev.v[4];
    virial[5] += ev.v[5];
  }

  if (vflag_fdotr) pair_virial_fdotr_compute(this);

  if (eflag_atom) {
    k_eatom.template modify<DeviceType>();
    k_eatom.template sync<LMPHostType>();
  }

  if (vflag_atom) {
    if (need_dup)
      Kokkos::Experimental::contribute(d_vatom, dup_vatom);
    k_vatom.template modify<DeviceType>();
    k_vatom.template sync<LMPHostType>();
  }

  atomKK->modified(execution_space,F_MASK);

  copymode = 0;

  // free duplicated memory
  if (need_dup) {
    dup_f     = decltype(dup_f)();
    dup_vatom = decltype(dup_vatom)();
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeNeigh,const typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeNeigh>::member_type& team) const
{
  const int ii = team.league_rank();
  const int i = d_ilist[ii + chunk_offset];
  const int itype = type[i];
  const X_FLOAT xtmp = x(i,0);
  const X_FLOAT ytmp = x(i,1);
  const X_FLOAT ztmp = x(i,2);
  const int jnum = d_numneigh[i];
  const int mu_i = d_map(type(i));

  // get a pointer to scratch memory
  // This is used to cache whether or not an atom is within the cutoff
  // If it is, inside is assigned to 1, otherwise -1
  const int team_rank = team.team_rank();
  const int scratch_shift = team_rank * maxneigh; // offset into pointer for entire team
  int* inside = (int*)team.team_shmem().get_shmem(team.team_size() * maxneigh * sizeof(int), 0) + scratch_shift;

  // loop over list of all neighbors within force cutoff
  // distsq[] = distance sq to each
  // rlist[] = distance vector to each
  // nearest[] = atom indices of neighbors

  int ncount = 0;
  Kokkos::parallel_reduce(Kokkos::TeamThreadRange(team,jnum),
      [&] (const int jj, int& count) {
    int j = d_neighbors(i,jj);
    j &= NEIGHMASK;

    const int jtype = type(j);

    const F_FLOAT delx = xtmp - x(j,0);
    const F_FLOAT dely = ytmp - x(j,1);
    const F_FLOAT delz = ztmp - x(j,2);
    const F_FLOAT rsq = delx*delx + dely*dely + delz*delz;

    inside[jj] = -1;
    if (rsq < d_cutsq(itype,jtype)) {
     inside[jj] = 1;
     count++;
    }
  },ncount);

  d_ncount(ii) = ncount;

  Kokkos::parallel_scan(Kokkos::TeamThreadRange(team,jnum),
      [&] (const int jj, int& offset, bool final) {

    if (inside[jj] < 0) return;

    if (final) {
      int j = d_neighbors(i,jj);
      j &= NEIGHMASK;
      const F_FLOAT delx = xtmp - x(j,0);
      const F_FLOAT dely = ytmp - x(j,1);
      const F_FLOAT delz = ztmp - x(j,2);
      const F_FLOAT rsq = delx*delx + dely*dely + delz*delz;
      const F_FLOAT r = sqrt(rsq);
      const F_FLOAT rinv = 1.0/r;
      const int mu_j = d_map(type(j));
      d_mu(ii,offset) = mu_j;
      d_rnorms(ii,offset) = r;
      d_rhats(ii,offset,0) = -delx*rinv;
      d_rhats(ii,offset,1) = -dely*rinv;
      d_rhats(ii,offset,2) = -delz*rinv;
      d_nearest(ii,offset) = j;
    }
    offset++;
  });

  if (is_zbl) {
    //adapted from https://www.osti.gov/servlets/purl/1429450
    if (ncount > 0) {
      using minloc_value_type=Kokkos::MinLoc<F_FLOAT,int>::value_type;
      minloc_value_type djjmin;
      djjmin.val=1e20;
      djjmin.loc=-1;
      Kokkos::MinLoc<F_FLOAT,int> reducer_scalar(djjmin);
      // loop over ncount (actual neighbours withing cutoff) rather than jnum (total number of neigh in cutoff+skin)
      Kokkos::parallel_reduce(Kokkos::TeamThreadRange(team, ncount),
               [&](const int offset, minloc_value_type &min_d_dist) {
                 int j = d_nearest(ii,offset);
                 j &= NEIGHMASK;
                 auto r = d_rnorms(ii,offset);
                 const int mu_j = d_map(type(j));
                 const F_FLOAT d = r - (d_cut_in(mu_i, mu_j) - d_dcut_in(mu_i, mu_j));
                 if (d < min_d_dist.val) {
                   min_d_dist.val = d;
                   min_d_dist.loc = offset;
                 }
       }, reducer_scalar);
      d_d_min(ii) = djjmin.val;
      d_jj_min(ii) = djjmin.loc;// d_jj_min should be NOT in 0..jnum range, but in 0..d_ncount(<=jnum)
    } else {
      d_d_min(ii) = 1e20;
      d_jj_min(ii) = -1;
    }
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeRadial, const typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeRadial>::member_type& team) const
{
  // Extract the atom number
  int ii = team.team_rank() + team.team_size() * (team.league_rank() %
           ((chunk_size+team.team_size()-1)/team.team_size()));
  if (ii >= chunk_size) return;
  const int i = d_ilist[ii + chunk_offset];

  // Extract the neighbor number
  const int jj = team.league_rank() / ((chunk_size+team.team_size()-1)/team.team_size());
  const int ncount = d_ncount(ii);
  if (jj >= ncount) return;

  const double r_norm = d_rnorms(ii, jj);
  const int mu_i = d_map(type(i));
  const int mu_j = d_mu(ii, jj);

  evaluate_splines(ii, jj, r_norm, nradbase, nradmax, mu_i, mu_j);
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeAi, const typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeAi>::member_type& team) const
{
  // Extract the atom number
  int ii = team.team_rank() + team.team_size() * (team.league_rank() %
           ((chunk_size+team.team_size()-1)/team.team_size()));
  if (ii >= chunk_size) return;

  // Extract the neighbor number
  const int jj = team.league_rank() / ((chunk_size+team.team_size()-1)/team.team_size());
  const int ncount = d_ncount(ii);
  if (jj >= ncount) return;

  const int mu_j = d_mu(ii, jj);

  // rank = 1
  for (int n = 0; n < nradbase; n++)
    Kokkos::atomic_add(&A_rank1(ii, mu_j, n), gr(ii, jj, n) * Y00);

  // rank > 1

  // Compute plm and ylm

  // requires rx^2 + ry^2 + rz^2 = 1 , NO CHECKING IS PERFORMED !!!!!!!!!
  // requires -1 <= rz <= 1 , NO CHECKING IS PERFORMED !!!!!!!!!
  // prefactors include 1/sqrt(2) factor compared to reference

  complex ylm, phase;
  complex phasem, mphasem1;
  complex dyx, dyy, dyz;
  complex rdy;

  const double rx = d_rhats(ii, jj, 0);
  const double ry = d_rhats(ii, jj, 1);
  const double rz = d_rhats(ii, jj, 2);

  phase.re = rx;
  phase.im = ry;

  double plm_idx,plm_idx1,plm_idx2;

  plm_idx = plm_idx1 = plm_idx2 = 0.0;

  int idx_sph = 0;

  // m = 0
  for (int l = 0; l <= lmax; l++) {
    // const int idx = l * (l + 1);

    if (l == 0) {
      // l=0, m=0
      // plm[0] = Y00/sq1o4pi; //= sq1o4pi;
      plm_idx = Y00; //= 1;
    } else if (l == 1) {
      // l=1, m=0
      plm_idx = Y00 * sq3 * rz;
    } else {
      // l>=2, m=0
      plm_idx = alm(idx_sph) * (rz * plm_idx1 + blm(idx_sph) * plm_idx2);
    }

    ylm.re = plm_idx;
    ylm.im = 0.0;

    for (int n = 0; n < nradmax; n++) {
      Kokkos::atomic_add(&A_sph(ii, mu_j, idx_sph, n).re, fr(ii, jj, l, n) * ylm.re);
      Kokkos::atomic_add(&A_sph(ii, mu_j, idx_sph, n).im, fr(ii, jj, l, n) * ylm.im);
    }

    plm_idx2 = plm_idx1;
    plm_idx1 = plm_idx;

    idx_sph++;
  }

  plm_idx = plm_idx1 = plm_idx2 = 0.0;

  // m = 1
  for (int l = 1; l <= lmax; l++) {
    // const int idx = l * (l + 1) + 1; // (l, 1)

    if (l == 1) {
      // l=1, m=1
      plm_idx = -sq3o2 * Y00;
    } else if (l == 2) {
      const double t = dl(l) * plm_idx1;
      plm_idx = t * rz;
    } else {
      plm_idx = alm(idx_sph) * (rz * plm_idx1 + blm(idx_sph) * plm_idx2);
    }

    ylm = phase * plm_idx;

    for (int n = 0; n < nradmax; n++) {
      Kokkos::atomic_add(&A_sph(ii, mu_j, idx_sph, n).re, fr(ii, jj, l, n) * ylm.re);
      Kokkos::atomic_add(&A_sph(ii, mu_j, idx_sph, n).im, fr(ii, jj, l, n) * ylm.im);
    }

    plm_idx2 = plm_idx1;
    plm_idx1 = plm_idx;

    idx_sph++;
  }

  plm_idx = plm_idx1 = plm_idx2 = 0.0;

  double plm_mm1_mm1 = -sq3o2 * Y00; // (1, 1)

  // m > 1
  phasem = phase;
  for (int m = 2; m <= lmax; m++) {

    mphasem1.re = phasem.re * double(m);
    mphasem1.im = phasem.im * double(m);
    phasem = phasem * phase;

    for (int l = m; l <= lmax; l++) {
      // const int idx = l * (l + 1) + m;

      if (l == m) {
        plm_idx = cl(l) * plm_mm1_mm1; // (m+1, m)
        plm_mm1_mm1 = plm_idx;
      } else if (l == (m + 1)) {
        const double t = dl(l) * plm_mm1_mm1; // (m - 1, m - 1)
        plm_idx = t * rz; // (m, m)
      } else {
        plm_idx = alm(idx_sph) * (rz * plm_idx1 + blm(idx_sph) * plm_idx2);
      }

      ylm.re = phasem.re * plm_idx;
      ylm.im = phasem.im * plm_idx;

      for (int n = 0; n < nradmax; n++) {
        Kokkos::atomic_add(&A_sph(ii, mu_j, idx_sph, n).re, fr(ii, jj, l, n) * ylm.re);
        Kokkos::atomic_add(&A_sph(ii, mu_j, idx_sph, n).im, fr(ii, jj, l, n) * ylm.im);
      }

      plm_idx2 = plm_idx1;
      plm_idx1 = plm_idx;

      idx_sph++;
    }
  }

  // hard-core repulsion
  Kokkos::atomic_add(&rho_core(ii), cr(ii, jj));
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEConjugateAi, const int& ii) const
{
  for (int mu_j = 0; mu_j < nelements; mu_j++) {

    // transpose

    int idx_sph = 0;

    for (int m = 0; m <= lmax; m++) {
      for (int l = m; l <= lmax; l++) {
        const int idx = l * (l + 1) + m;
        for (int n = 0; n < nradmax; n++) {
          A(ii, mu_j, idx, n) = A_sph(ii, mu_j, idx_sph, n);
        }

        idx_sph++;
      }
    }

    // complex conjugate A's (for NEGATIVE (-m) terms)
    //  for rank > 1

    for (int l = 0; l <= lmax; l++) {
        //fill in -m part in the outer loop using the same m <-> -m symmetry as for Ylm
      for (int m = 1; m <= l; m++) {
        const int idx = l * (l + 1) + m; // (l, m)
        const int idxm = l * (l + 1) - m; // (l, -m)
        const int idx_sph = d_idx_sph(idx);
        const int factor = m % 2 == 0 ? 1 : -1;
        for (int n = 0; n < nradmax; n++) {
          A(ii, mu_j, idxm, n) = A_sph(ii, mu_j, idx_sph, n).conj() * (double)factor;
        }
      }
    }
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeRho, const int& iter) const
{
  const int idx_ms_combs = iter / chunk_size;
  const int ii = iter % chunk_size;

  const int i = d_ilist[ii + chunk_offset];
  const int mu_i = d_map(type(i));

  if (idx_ms_combs >= d_idx_ms_combs_count(mu_i)) return;

  const int ndensity = d_ndensity(mu_i);

  const int idx_func = d_idx_funcs(mu_i, idx_ms_combs);
  const int rank = d_rank(mu_i, idx_func);
  const int r = rank - 1;

  // Basis functions B with iterative product and density rho(p) calculation
  if (rank == 1) {
    const int mu = d_mus(mu_i, idx_func, 0);
    const int n = d_ns(mu_i, idx_func, 0);
    double A_cur = A_rank1(ii, mu, n - 1);
    for (int p = 0; p < ndensity; ++p) {
      //for rank=1 (r=0) only 1 ms-combination exists (ms_ind=0), so index of func.ctildes is 0..ndensity-1
      Kokkos::atomic_add(&rhos(ii, p), d_coeffs(mu_i, idx_func, p) * d_gen_cgs(mu_i, idx_ms_combs) * A_cur);
    }

    // gamma_i
    if (flag_compute_extrapolation_grade)
      Kokkos::atomic_add(&projections(ii, idx_func),  d_gen_cgs(mu_i, idx_ms_combs) * A_cur);

  } else { // rank > 1
    // loop over {ms} combinations in sum

    // loop over m, collect B  = product of A with given ms
    A_forward_prod(ii, idx_ms_combs, 0) = complex::one();

    // fill forward A-product triangle
    for (int t = 0; t < rank; t++) {
      //TODO: optimize ns[t]-1 -> ns[t] during functions construction
      const int mu = d_mus(mu_i, idx_func, t);
      const int n = d_ns(mu_i, idx_func, t);
      const int l = d_ls(mu_i, idx_func, t);
      const int m = d_ms_combs(mu_i, idx_ms_combs, t); // current ms-combination (of length = rank)
      const int idx = l * (l + 1) + m; // (l, m)
      A_list(ii, idx_ms_combs, t) = A(ii, mu, idx, n - 1);
      A_forward_prod(ii, idx_ms_combs, t + 1) = A_forward_prod(ii, idx_ms_combs, t) * A_list(ii, idx_ms_combs, t);
    }

    complex A_backward_prod = complex::one();

    // fill backward A-product triangle
    for (int t = r; t >= 1; t--) {
      const complex dB = A_forward_prod(ii, idx_ms_combs, t) * A_backward_prod; // dB - product of all A's except t-th
      dB_flatten(ii, idx_ms_combs, t) = dB;

      A_backward_prod = A_backward_prod * A_list(ii, idx_ms_combs, t);
    }
    dB_flatten(ii, idx_ms_combs, 0) = A_forward_prod(ii, idx_ms_combs, 0) * A_backward_prod;

    const complex B = A_forward_prod(ii, idx_ms_combs, rank);

    for (int p = 0; p < ndensity; ++p) {
      // real-part only multiplication
      Kokkos::atomic_add(&rhos(ii, p), B.real_part_product(d_coeffs(mu_i, idx_func, p) * d_gen_cgs(mu_i, idx_ms_combs)));
    }
    // gamma_i
    if (flag_compute_extrapolation_grade)
      Kokkos::atomic_add(&projections(ii, idx_func),  B.real_part_product(d_gen_cgs(mu_i, idx_ms_combs)));
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeFS, const int& ii) const
{
  const int i = d_ilist[ii + chunk_offset];
  const int mu_i = d_map(type(i));

  const double rho_cut = d_rho_core_cutoff(mu_i);
  const double drho_cut = d_drho_core_cutoff(mu_i);
  const int ndensity = d_ndensity(mu_i);

  double evdwl, fcut, dfcut;
  double evdwl_cut;
  evdwl = fcut = dfcut = 0.0;

  FS_values_and_derivatives(ii, evdwl, mu_i);

  if (is_zbl) {
    if (d_jj_min(ii) != -1) {
      const int mu_jmin = d_mu(ii,d_jj_min(ii));
      F_FLOAT dcutin = d_dcut_in(mu_i, mu_jmin);
      F_FLOAT transition_coordinate =  dcutin  - d_d_min(ii); // == cutin - r_min
      cutoff_func_poly(transition_coordinate, dcutin, dcutin, fcut, dfcut);
      dfcut = -dfcut; // invert, because rho_core = cutin - r_min
    } else {
      // no neighbours
      fcut = 1;
      dfcut = 0;
    }
    evdwl_cut = evdwl * fcut + rho_core(ii) * (1 - fcut); // evdwl * fcut + rho_core_uncut  - rho_core_uncut* fcut
    dF_drho_core(ii) = 1 - fcut;
    dF_dfcut(ii) = evdwl * dfcut - rho_core(ii) * dfcut;
  } else {
    inner_cutoff(rho_core(ii), rho_cut, drho_cut, fcut, dfcut);
    dF_drho_core(ii) = evdwl * dfcut + 1;
    evdwl_cut = evdwl * fcut + rho_core(ii);
  }
  for (int p = 0; p < ndensity; ++p)
    dF_drho(ii, p) *= fcut;

  // tally energy contribution
  if (eflag) {
    // E0 shift
    evdwl_cut += d_E0vals(mu_i);
    e_atom(ii) = evdwl_cut;
  }

  if (flag_corerep_factor)
    d_corerep(ii) = 1-fcut;
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeGamma, const int& ii) const
{
  const int i = d_ilist[ii + chunk_offset];
  const int mu_i = d_map(type(i));
  const int basis_size = d_total_basis_size(mu_i);

  double gamma_max = 0;
  double current_gamma;
  for (int j = 0; j <basis_size; j++) {
    current_gamma = 0;

    // compute row-matrix-multiplication asi_vector * A_as_inv (transposed matrix)
    for (int k = 0; k < basis_size; k++)
      current_gamma += projections(ii,k) * d_ASI(mu_i, k, j); //correct d_ASI(mu_i, j, k), but it is transposed during initialization

    current_gamma = fabs(current_gamma);
    if (current_gamma > gamma_max)
      gamma_max = current_gamma;
  }

  // tally energy contribution
  d_gamma(ii) = gamma_max;
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeWeights, const int& iter) const
{
  const int idx_ms_combs = iter / chunk_size;
  const int ii = iter % chunk_size;

  const int i = d_ilist[ii + chunk_offset];
  const int mu_i = d_map(type(i));

  if (idx_ms_combs >= d_idx_ms_combs_count(mu_i)) return;

  const int ndensity = d_ndensity(mu_i);

  const int idx_func = d_idx_funcs(mu_i, idx_ms_combs);
  const int rank = d_rank(mu_i, idx_func);

  // Weights and theta calculation

  if (rank == 1) {
    const int mu = d_mus(mu_i, idx_func, 0);
    const int n = d_ns(mu_i, idx_func, 0);
    double theta = 0.0;
    for (int p = 0; p < ndensity; ++p) {
      // for rank=1 (r=0) only 1 ms-combination exists (ms_ind=0), so index of func.ctildes is 0..ndensity-1
      theta += dF_drho(ii, p) * d_coeffs(mu_i, idx_func, p) * d_gen_cgs(mu_i, idx_ms_combs);
    }
    Kokkos::atomic_add(&weights_rank1(ii, mu, n - 1), theta);
  } else { // rank > 1
    double theta = 0.0;
    for (int p = 0; p < ndensity; ++p)
      theta += dF_drho(ii, p) * d_coeffs(mu_i, idx_func, p) * d_gen_cgs(mu_i, idx_ms_combs);

    theta *= 0.5; // 0.5 factor due to possible double counting ???
    for (int t = 0; t < rank; ++t) {
      const int m_t = d_ms_combs(mu_i, idx_ms_combs, t);
      const int factor = (m_t % 2 == 0 ? 1 : -1);
      const complex dB = dB_flatten(ii, idx_ms_combs, t);
      const int mu_t = d_mus(mu_i, idx_func, t);
      const int n_t = d_ns(mu_i, idx_func, t);
      const int l_t = d_ls(mu_i, idx_func, t);
      const int idx = l_t * (l_t + 1) + m_t; // (l, m)
      const int idx_sph = d_idx_sph(idx);
      if (idx_sph >= 0) {
        const complex value = theta * dB;
        Kokkos::atomic_add(&(weights(ii, mu_t, idx_sph, n_t - 1).re), value.re);
        Kokkos::atomic_add(&(weights(ii, mu_t, idx_sph, n_t - 1).im), value.im);
      }
      // update -m_t (that could also be positive), because the basis is half_basis
      const int idxm = l_t * (l_t + 1) - m_t; // (l, -m)
      const int idxm_sph = d_idx_sph(idxm);
      if (idxm_sph >= 0) {
        const complex valuem = theta * dB.conj() * (double)factor;
        Kokkos::atomic_add(&(weights(ii, mu_t, idxm_sph, n_t - 1).re), valuem.re);
        Kokkos::atomic_add(&(weights(ii, mu_t, idxm_sph, n_t - 1).im), valuem.im);
      }
    }
  }
}

/* ---------------------------------------------------------------------- */
template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeDerivative, const typename Kokkos::TeamPolicy<DeviceType, TagPairPACEComputeDerivative>::member_type& team) const
{
  // Extract the atom number
  int ii = team.team_rank() + team.team_size() * (team.league_rank() %
           ((chunk_size+team.team_size()-1)/team.team_size()));
  if (ii >= chunk_size) return;
  const int i = d_ilist[ii + chunk_offset];

  // Extract the neighbor number
  const int jj = team.league_rank() / ((chunk_size+team.team_size()-1)/team.team_size());
  const int ncount = d_ncount(ii);
  if (jj >= ncount) return;

  const int itype = type(i);
  const double scale = d_scale(itype,itype);

  const int mu_j = d_mu(ii, jj);
  double r_hat[3];
  r_hat[0] = d_rhats(ii, jj, 0);
  r_hat[1] = d_rhats(ii, jj, 1);
  r_hat[2] = d_rhats(ii, jj, 2);
  const double r = d_rnorms(ii, jj);
  const double rinv = 1.0/r;

  double f_ji[3];
  f_ji[0] = f_ji[1] = f_ji[2] = 0;

  // for rank = 1
  for (int n = 0; n < nradbase; ++n) {
    if (weights_rank1(ii, mu_j, n) == 0) continue;
    double &DG = dgr(ii, jj, n);
    double DGR = DG * Y00;
    DGR *= weights_rank1(ii, mu_j, n);
    f_ji[0] += DGR * r_hat[0];
    f_ji[1] += DGR * r_hat[1];
    f_ji[2] += DGR * r_hat[2];
  }

  // for rank > 1

  // compute plm, dplm, ylm and dylm
  // requires rx^2 + ry^2 + rz^2 = 1 , NO CHECKING IS PERFORMED !!!!!!!!!
  // requires -1 <= rz <= 1 , NO CHECKING IS PERFORMED !!!!!!!!!
  // prefactors include 1/sqrt(2) factor compared to reference

  complex ylm,dylm[3];
  complex phase;
  complex phasem, mphasem1;
  complex dyx, dyy, dyz;
  complex rdy;

  const double rx = d_rhats(ii, jj, 0);
  const double ry = d_rhats(ii, jj, 1);
  const double rz = d_rhats(ii, jj, 2);

  phase.re = rx;
  phase.im = ry;

  double plm_idx,plm_idx1,plm_idx2;
  double dplm_idx,dplm_idx1,dplm_idx2;

  plm_idx = plm_idx1 = plm_idx2 = 0.0;
  dplm_idx = dplm_idx1 = dplm_idx2 = 0.0;

  int idx_sph = 0;

  // m = 0
  for (int l = 0; l <= lmax; l++) {
    // const int idx = l * (l + 1);

    if (l == 0) {
      // l=0, m=0
      // plm[0] = Y00/sq1o4pi; //= sq1o4pi;
      plm_idx = Y00; //= 1;
      dplm_idx = 0.0;
    } else if (l == 1) {
      // l=1, m=0
      plm_idx = Y00 * sq3 * rz;
      dplm_idx = Y00 * sq3;
    } else {
      // l>=2, m=0
      plm_idx = alm(idx_sph) * (rz * plm_idx1 + blm(idx_sph) * plm_idx2);
      dplm_idx = alm(idx_sph) * (plm_idx1 + rz * dplm_idx1 + blm(idx_sph) * dplm_idx2);
    }

    ylm.re = plm_idx;
    ylm.im = 0.0;

    dyz.re = dplm_idx;
    rdy.re = dyz.re * rz;

    dylm[0].re = -rdy.re * rx;
    dylm[0].im = 0.0;
    dylm[1].re = -rdy.re * ry;
    dylm[1].im = 0.0;
    dylm[2].re = dyz.re - rdy.re * rz;
    dylm[2].im = 0;

    for (int n = 0; n < nradmax; n++) {

      const double R_over_r = fr(ii, jj, l, n) * rinv;
      const double DR = dfr(ii, jj, l, n);
      const complex Y_DR = ylm * DR;

      complex w = weights(ii, mu_j, idx_sph, n);
      if (w.re == 0.0 && w.im == 0.0) continue;

      complex grad_phi_nlm[3];
      grad_phi_nlm[0] = Y_DR * r_hat[0] + dylm[0] * R_over_r;
      grad_phi_nlm[1] = Y_DR * r_hat[1] + dylm[1] * R_over_r;
      grad_phi_nlm[2] = Y_DR * r_hat[2] + dylm[2] * R_over_r;
      // real-part multiplication only
      f_ji[0] += w.real_part_product(grad_phi_nlm[0]);
      f_ji[1] += w.real_part_product(grad_phi_nlm[1]);
      f_ji[2] += w.real_part_product(grad_phi_nlm[2]);
    }

    plm_idx2 = plm_idx1;
    dplm_idx2 = dplm_idx1;

    plm_idx1 = plm_idx;
    dplm_idx1 = dplm_idx;

    idx_sph++;
  }

  plm_idx = plm_idx1 = plm_idx2 = 0.0;
  dplm_idx = dplm_idx1 = dplm_idx2 = 0.0;

  // m = 1
  for (int l = 1; l <= lmax; l++) {
    // const int idx = l * (l + 1) + 1; // (l, 1)

    if (l == 1) {
      // l=1, m=1
      plm_idx = -sq3o2 * Y00;
      dplm_idx = 0.0;
    } else if (l == 2) {
      const double t = dl(l) * plm_idx1;
      plm_idx = t * rz;
      dplm_idx = t;
    } else {
      plm_idx = alm(idx_sph) * (rz * plm_idx1 + blm(idx_sph) * plm_idx2);
      dplm_idx = alm(idx_sph) * (plm_idx1 + rz * dplm_idx1 + blm(idx_sph) * dplm_idx2);
    }

    ylm = phase * plm_idx;

    dyx.re = plm_idx;
    dyx.im = 0.0;
    dyy.re = 0.0;
    dyy.im = plm_idx;
    dyz.re = phase.re * dplm_idx;
    dyz.im = phase.im * dplm_idx;

    rdy.re = rx * dyx.re + +rz * dyz.re;
    rdy.im = ry * dyy.im + rz * dyz.im;

    dylm[0].re = dyx.re - rdy.re * rx;
    dylm[0].im = -rdy.im * rx;
    dylm[1].re = -rdy.re * ry;
    dylm[1].im = dyy.im - rdy.im * ry;
    dylm[2].re = dyz.re - rdy.re * rz;
    dylm[2].im = dyz.im - rdy.im * rz;

    for (int n = 0; n < nradmax; n++) {

      const double R_over_r = fr(ii, jj, l, n) * rinv;
      const double DR = dfr(ii, jj, l, n);
      const complex Y_DR = ylm * DR;

      complex w = weights(ii, mu_j, idx_sph, n);
      if (w.re == 0.0 && w.im == 0.0) continue;
      // counting for -m cases if m > 0
      w.re *= 2.0;
      w.im *= 2.0;

      complex grad_phi_nlm[3];
      grad_phi_nlm[0] = Y_DR * r_hat[0] + dylm[0] * R_over_r;
      grad_phi_nlm[1] = Y_DR * r_hat[1] + dylm[1] * R_over_r;
      grad_phi_nlm[2] = Y_DR * r_hat[2] + dylm[2] * R_over_r;
      // real-part multiplication only
      f_ji[0] += w.real_part_product(grad_phi_nlm[0]);
      f_ji[1] += w.real_part_product(grad_phi_nlm[1]);
      f_ji[2] += w.real_part_product(grad_phi_nlm[2]);
    }

    plm_idx2 = plm_idx1;
    dplm_idx2 = dplm_idx1;

    plm_idx1 = plm_idx;
    dplm_idx1 = dplm_idx;

    idx_sph++;
  }

  plm_idx = plm_idx1 = plm_idx2 = 0.0;
  dplm_idx = dplm_idx1 = dplm_idx2 = 0.0;

  double plm_mm1_mm1 = -sq3o2 * Y00; // (1, 1)

  // m > 1
  phasem = phase;
  for (int m = 2; m <= lmax; m++) {

    mphasem1.re = phasem.re * double(m);
    mphasem1.im = phasem.im * double(m);
    phasem = phasem * phase;

    for (int l = m; l <= lmax; l++) {
      // const int idx = l * (l + 1) + m;

      if (l == m) {
        plm_idx = cl(l) * plm_mm1_mm1; // (m+1, m)
        dplm_idx = 0.0;
        plm_mm1_mm1 = plm_idx;
      } else if (l == (m + 1)) {
        const double t = dl(l) * plm_mm1_mm1; // (m - 1, m - 1)
        plm_idx = t * rz; // (m, m)
        dplm_idx = t;
      } else {
        plm_idx = alm(idx_sph) * (rz * plm_idx1 + blm(idx_sph) * plm_idx2);
        dplm_idx = alm(idx_sph) * (plm_idx1 + rz * dplm_idx1 + blm(idx_sph) * dplm_idx2);
      }

      ylm.re = phasem.re * plm_idx;
      ylm.im = phasem.im * plm_idx;

      dyx = mphasem1 * plm_idx;
      dyy.re = -dyx.im;
      dyy.im = dyx.re;
      dyz = phasem * dplm_idx;

      rdy.re = rx * dyx.re + ry * dyy.re + rz * dyz.re;
      rdy.im = rx * dyx.im + ry * dyy.im + rz * dyz.im;

      dylm[0].re = dyx.re - rdy.re * rx;
      dylm[0].im = dyx.im - rdy.im * rx;
      dylm[1].re = dyy.re - rdy.re * ry;
      dylm[1].im = dyy.im - rdy.im * ry;
      dylm[2].re = dyz.re - rdy.re * rz;
      dylm[2].im = dyz.im - rdy.im * rz;

      for (int n = 0; n < nradmax; n++) {

        const double R_over_r = fr(ii, jj, l, n) * rinv;
        const double DR = dfr(ii, jj, l, n);
        const complex Y_DR = ylm * DR;

        complex w = weights(ii, mu_j, idx_sph, n);
        if (w.re == 0.0 && w.im == 0.0) continue;
        // counting for -m cases if m > 0
        w.re *= 2.0;
        w.im *= 2.0;

        complex grad_phi_nlm[3];
        grad_phi_nlm[0] = Y_DR * r_hat[0] + dylm[0] * R_over_r;
        grad_phi_nlm[1] = Y_DR * r_hat[1] + dylm[1] * R_over_r;
        grad_phi_nlm[2] = Y_DR * r_hat[2] + dylm[2] * R_over_r;
        // real-part multiplication only
        f_ji[0] += w.real_part_product(grad_phi_nlm[0]);
        f_ji[1] += w.real_part_product(grad_phi_nlm[1]);
        f_ji[2] += w.real_part_product(grad_phi_nlm[2]);
      }

      plm_idx2 = plm_idx1;
      dplm_idx2 = dplm_idx1;

      plm_idx1 = plm_idx;
      dplm_idx1 = dplm_idx;

      idx_sph++;
    }
  }

  // hard-core repulsion
  const double fpair = dF_drho_core(ii) * dcr(ii,jj);
  f_ij(ii, jj, 0) = scale * f_ji[0] + fpair * r_hat[0];
  f_ij(ii, jj, 1) = scale * f_ji[1] + fpair * r_hat[1];
  f_ij(ii, jj, 2) = scale * f_ji[2] + fpair * r_hat[2];

  if (is_zbl) {
    if (jj==d_jj_min(ii)) {
      // DCRU = 1.0
      f_ij(ii, jj, 0) += dF_dfcut(ii) * r_hat[0];
      f_ij(ii, jj, 1) += dF_dfcut(ii) * r_hat[1];
      f_ij(ii, jj, 2) += dF_dfcut(ii) * r_hat[2];
    }
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
template<int NEIGHFLAG, int EVFLAG>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeForce<NEIGHFLAG,EVFLAG>, const int& ii, EV_FLOAT& ev) const
{
  // The f array is duplicated for OpenMP, atomic for GPU, and neither for Serial
  const auto v_f = ScatterViewHelper<NeedDup_v<NEIGHFLAG,DeviceType>,decltype(dup_f),decltype(ndup_f)>::get(dup_f,ndup_f);
  const auto a_f = v_f.template access<AtomicDup_v<NEIGHFLAG,DeviceType>>();

  const int i = d_ilist[ii + chunk_offset];
  const int itype = type(i);
  const double scale = d_scale(itype,itype);

  const int ncount = d_ncount(ii);

  F_FLOAT fitmp[3] = {0.0,0.0,0.0};
  for (int jj = 0; jj < ncount; jj++) {
    int j = d_nearest(ii,jj);

    double r_hat[3];
    r_hat[0] = d_rhats(ii, jj, 0);
    r_hat[1] = d_rhats(ii, jj, 1);
    r_hat[2] = d_rhats(ii, jj, 2);
    const double r = d_rnorms(ii, jj);
    const double delx = -r_hat[0]*r;
    const double dely = -r_hat[1]*r;
    const double delz = -r_hat[2]*r;

    const double fpairx = f_ij(ii, jj, 0);
    const double fpairy = f_ij(ii, jj, 1);
    const double fpairz = f_ij(ii, jj, 2);

    fitmp[0] += fpairx;
    fitmp[1] += fpairy;
    fitmp[2] += fpairz;
    a_f(j,0) -= fpairx;
    a_f(j,1) -= fpairy;
    a_f(j,2) -= fpairz;

    // tally per-atom virial contribution
    if (EVFLAG && vflag_either)
      v_tally_xyz<NEIGHFLAG>(ev, i, j, fpairx, fpairy, fpairz, delx, dely, delz);
  }

  a_f(i,0) += fitmp[0];
  a_f(i,1) += fitmp[1];
  a_f(i,2) += fitmp[2];

  // tally energy contribution
  if (EVFLAG && eflag_either) {
    const double evdwl = scale*e_atom(ii);
    //ev_tally_full(i, 2.0 * evdwl, 0.0, 0.0, 0.0, 0.0, 0.0);
    if (eflag_global) ev.evdwl += evdwl;
    if (eflag_atom) d_eatom[i] += evdwl;
  }
}

template<class DeviceType>
template<int NEIGHFLAG, int EVFLAG>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::operator() (TagPairPACEComputeForce<NEIGHFLAG,EVFLAG>,const int& ii) const {
  EV_FLOAT ev;
  this->template operator()<NEIGHFLAG,EVFLAG>(TagPairPACEComputeForce<NEIGHFLAG,EVFLAG>(), ii, ev);
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
template<int NEIGHFLAG>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::v_tally_xyz(EV_FLOAT &ev, const int &i, const int &j,
      const F_FLOAT &fx, const F_FLOAT &fy, const F_FLOAT &fz,
      const F_FLOAT &delx, const F_FLOAT &dely, const F_FLOAT &delz) const
{
  // The vatom array is duplicated for OpenMP, atomic for GPU, and neither for Serial

  auto v_vatom = ScatterViewHelper<NeedDup_v<NEIGHFLAG,DeviceType>,decltype(dup_vatom),decltype(ndup_vatom)>::get(dup_vatom,ndup_vatom);
  auto a_vatom = v_vatom.template access<AtomicDup_v<NEIGHFLAG,DeviceType>>();

  const E_FLOAT v0 = delx*fx;
  const E_FLOAT v1 = dely*fy;
  const E_FLOAT v2 = delz*fz;
  const E_FLOAT v3 = delx*fy;
  const E_FLOAT v4 = delx*fz;
  const E_FLOAT v5 = dely*fz;

  if (vflag_global) {
    ev.v[0] += v0;
    ev.v[1] += v1;
    ev.v[2] += v2;
    ev.v[3] += v3;
    ev.v[4] += v4;
    ev.v[5] += v5;
  }

  if (vflag_atom) {
    a_vatom(i,0) += 0.5*v0;
    a_vatom(i,1) += 0.5*v1;
    a_vatom(i,2) += 0.5*v2;
    a_vatom(i,3) += 0.5*v3;
    a_vatom(i,4) += 0.5*v4;
    a_vatom(i,5) += 0.5*v5;
    a_vatom(j,0) += 0.5*v0;
    a_vatom(j,1) += 0.5*v1;
    a_vatom(j,2) += 0.5*v2;
    a_vatom(j,3) += 0.5*v3;
    a_vatom(j,4) += 0.5*v4;
    a_vatom(j,5) += 0.5*v5;
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::pre_compute_harmonics(int lmax)
{
  auto h_idx_sph = Kokkos::create_mirror_view(d_idx_sph);
  auto h_alm = Kokkos::create_mirror_view(alm);
  auto h_blm = Kokkos::create_mirror_view(blm);
  auto h_cl = Kokkos::create_mirror_view(cl);
  auto h_dl = Kokkos::create_mirror_view(dl);

  Kokkos::deep_copy(h_idx_sph,-1);

  int idx_sph = 0;
  for (int m = 0; m <= lmax; m++) {
    const double msq = m * m;
    for (int l = m; l <= lmax; l++) {
      const int idx = l * (l + 1) + m; // (l, m)
      h_idx_sph(idx) = idx_sph;

      double a = 0.0;
      double b = 0.0;

      if (l > 1 && l != m) {
        const double lsq = l * l;
        const double ld = 2 * l;
        const double l1 = (4 * lsq - 1);
        const double l2 = lsq - ld + 1;

        a = sqrt((double(l1)) / (double(lsq - msq)));
        b = -sqrt((double(l2 - msq)) / (double(4 * l2 - 1)));
      }
      h_alm(idx_sph) = a;
      h_blm(idx_sph) = b;
      idx_sph++;
    }
  }
  idx_sph_max = idx_sph;

  for (int l = 1; l <= lmax; l++) {
    h_cl(l) = -sqrt(1.0 + 0.5 / (double(l)));
    h_dl(l) = sqrt(double(2 * (l - 1) + 3));
  }

  Kokkos::deep_copy(d_idx_sph, h_idx_sph);
  Kokkos::deep_copy(alm, h_alm);
  Kokkos::deep_copy(blm, h_blm);
  Kokkos::deep_copy(cl, h_cl);
  Kokkos::deep_copy(dl, h_dl);
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::cutoff_func_poly(const double r, const double r_in, const double delta_in, double &fc, double &dfc) const
{
  if (r <= r_in-delta_in) {
    fc = 1;
    dfc = 0;
  } else if (r >= r_in ) {
    fc = 0;
    dfc = 0;
  } else {
    double x = 1 - 2 * (1 + (r - r_in) / delta_in);
    fc = 0.5 + 7.5 / 2. * (x / 4. - pow(x, 3) / 6. + pow(x, 5) / 20.);
    dfc = -7.5 / delta_in * (0.25 - x * x / 2.0 + pow(x, 4) / 4.);
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::Fexp(const double x, const double m, double &F, double &DF) const
{
  const double w = 1.e6;
  const double eps = 1e-10;

  const double lambda = pow(1.0 / w, m - 1.0);
  if (abs(x) > eps) {
    double g;
    const double a = abs(x);
    const double am = pow(a, m);
    const double w3x3 = pow(w * a, 3); //// use cube
    const double sign_factor = (signbit(x) ? -1 : 1);
    if (w3x3 > 30.0)
        g = 0.0;
    else
        g = exp(-w3x3);

    const double omg = 1.0 - g;
    F = sign_factor * (omg * am + lambda * g * a);
    const double dg = -3.0 * w * w * w * a * a * g;
    DF = m * pow(a, m - 1.0) * omg - am * dg + lambda * dg * a + lambda * g;
  } else {
    F = lambda * x;
    DF = lambda;
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::FexpShiftedScaled(const double rho, const double mexp, double &F, double &DF) const
{
  const double eps = 1e-10;

  if (abs(mexp - 1.0) < eps) {
    F = rho;
    DF = 1;
  } else {
    const double a = abs(rho);
    const double exprho = exp(-a);
    const double nx = 1. / mexp;
    const double xoff = pow(nx, (nx / (1.0 - nx))) * exprho;
    const double yoff = pow(nx, (1 / (1.0 - nx))) * exprho;
    const double sign_factor = (signbit(rho) ? -1 : 1);
    F = sign_factor * (pow(xoff + a, mexp) - yoff);
    DF = yoff + mexp * (-xoff + 1.0) * pow(xoff + a, mexp - 1.);
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::inner_cutoff(const double rho_core, const double rho_cut, const double drho_cut,
                                     double &fcut, double &dfcut) const
{
  double rho_low = rho_cut - drho_cut;
  if (rho_core >= rho_cut) {
    fcut = 0;
    dfcut = 0;
  } else if (rho_core <= rho_low) {
    fcut = 1;
    dfcut = 0;
  } else {
    cutoff_func_poly(rho_core, rho_cut, drho_cut, fcut, dfcut);
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::FS_values_and_derivatives(const int ii, double &evdwl, const int mu_i) const
{
  double F, DF = 0;
  int npoti = d_npoti(mu_i);
  int ndensity = d_ndensity(mu_i);
  for (int p = 0; p < ndensity; p++) {
    const double wpre = d_wpre(mu_i, p);
    const double mexp = d_mexp(mu_i, p);

    if (npoti == FS)
      Fexp(rhos(ii, p), mexp, F, DF);
    else if (npoti == FS_SHIFTEDSCALED)
      FexpShiftedScaled(rhos(ii, p), mexp, F, DF);

    evdwl += F * wpre; // * weight (wpre)
    dF_drho(ii, p) = DF * wpre; // * weight (wpre)
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::evaluate_splines(const int ii, const int jj, double r,
                                                  int /*nradbase_c*/, int /*nradial_c*/,
                                                  int mu_i, int mu_j) const
{
  auto &spline_gk = k_splines_gk.template view<DeviceType>()(mu_i, mu_j);
  auto &spline_rnl = k_splines_rnl.template view<DeviceType>()(mu_i, mu_j);
  auto &spline_hc = k_splines_hc.template view<DeviceType>()(mu_i, mu_j);

  spline_gk.calcSplines(ii, jj, r, gr, dgr);

  spline_rnl.calcSplines(ii, jj, r, d_values, d_derivatives);
  for (int ll = 0; ll < (int)fr.extent(2); ll++) {
    for (int kk = 0; kk < (int)fr.extent(3); kk++) {
      const int flatten = kk*fr.extent(2) + ll;
      fr(ii, jj, ll, kk) = d_values(ii, jj, flatten);
      dfr(ii, jj, ll, kk) = d_derivatives(ii, jj, flatten);
    }
  }

  spline_hc.calcSplines(ii, jj, r, d_values, d_derivatives);
  cr(ii, jj) = d_values(ii, jj, 0);
  dcr(ii, jj) = d_derivatives(ii, jj, 0);
}

/* ---------------------------------------------------------------------- */
template<class DeviceType>
void PairPACEExtrapolationKokkos<DeviceType>::SplineInterpolatorKokkos::operator=(const SplineInterpolator &spline) {
    cutoff = spline.cutoff;
    deltaSplineBins = spline.deltaSplineBins;
    ntot = spline.ntot;
    nlut = spline.nlut;
    invrscalelookup = spline.invrscalelookup;
    rscalelookup = spline.rscalelookup;
    num_of_functions = spline.num_of_functions;

    lookupTable = t_ace_3d4_lr("lookupTable", ntot+1, num_of_functions);
    auto h_lookupTable = Kokkos::create_mirror_view(lookupTable);
    for (int i = 0; i < ntot+1; i++)
        for (int j = 0; j < num_of_functions; j++)
            for (int k = 0; k < 4; k++)
                h_lookupTable(i, j, k) = spline.lookupTable(i, j, k);
    Kokkos::deep_copy(lookupTable, h_lookupTable);
}
/* ---------------------------------------------------------------------- */
template<class DeviceType>
KOKKOS_INLINE_FUNCTION
void PairPACEExtrapolationKokkos<DeviceType>::SplineInterpolatorKokkos::calcSplines(const int ii, const int jj, const double r, const t_ace_3d &d_values, const t_ace_3d &d_derivatives) const
{
  double wl, wl2, wl3, w2l1, w3l2;
  double c[4];
  double x = r * rscalelookup;
  int nl = static_cast<int>(floor(x));

  if (nl <= 0)
    Kokkos::abort("Encountered very small distance. Stopping.");

  if (nl < nlut) {
    wl = x - double(nl);
    wl2 = wl * wl;
    wl3 = wl2 * wl;
    w2l1 = 2.0 * wl;
    w3l2 = 3.0 * wl2;
    for (int func_id = 0; func_id < num_of_functions; func_id++) {
      for (int idx = 0; idx < 4; idx++)
        c[idx] = lookupTable(nl, func_id, idx);
      d_values(ii, jj, func_id) = c[0] + c[1] * wl + c[2] * wl2 + c[3] * wl3;
      d_derivatives(ii, jj, func_id) = (c[1] + c[2] * w2l1 + c[3] * w3l2) * rscalelookup;
    }
  } else { // fill with zeroes
    for (int func_id = 0; func_id < num_of_functions; func_id++) {
      d_values(ii, jj, func_id) = 0.0;
      d_derivatives(ii, jj, func_id) = 0.0;
    }
  }
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
template<class TagStyle>
void PairPACEExtrapolationKokkos<DeviceType>::check_team_size_for(int inum, int &team_size, int vector_length) {
  int team_size_max;

  team_size_max = Kokkos::TeamPolicy<DeviceType,TagStyle>(inum,Kokkos::AUTO).team_size_max(*this,Kokkos::ParallelForTag());

  if (team_size*vector_length > team_size_max)
    team_size = team_size_max/vector_length;
}

/* ---------------------------------------------------------------------- */

template<class DeviceType>
template<class TagStyle>
void PairPACEExtrapolationKokkos<DeviceType>::check_team_size_reduce(int inum, int &team_size, int vector_length) {
  int team_size_max;

  team_size_max = Kokkos::TeamPolicy<DeviceType,TagStyle>(inum,Kokkos::AUTO).team_size_max(*this,Kokkos::ParallelReduceTag());

  if (team_size*vector_length > team_size_max)
    team_size = team_size_max/vector_length;
}

template<class DeviceType>
template<typename scratch_type>
int PairPACEExtrapolationKokkos<DeviceType>::scratch_size_helper(int values_per_team) {
  typedef Kokkos::View<scratch_type*, Kokkos::DefaultExecutionSpace::scratch_memory_space, Kokkos::MemoryTraits<Kokkos::Unmanaged> > ScratchViewType;

  return ScratchViewType::shmem_size(values_per_team);
}

/* ----------------------------------------------------------------------
   memory usage of arrays
------------------------------------------------------------------------- */

template<class DeviceType>
double PairPACEExtrapolationKokkos<DeviceType>::memory_usage()
{
  double bytes = 0;

  bytes += MemKK::memory_usage(A);
  bytes += MemKK::memory_usage(A_rank1);
  bytes += MemKK::memory_usage(A_list);
  bytes += MemKK::memory_usage(A_forward_prod);
  bytes += MemKK::memory_usage(e_atom);
  bytes += MemKK::memory_usage(rhos);
  bytes += MemKK::memory_usage(dF_drho);
  bytes += MemKK::memory_usage(weights);
  bytes += MemKK::memory_usage(weights_rank1);
  bytes += MemKK::memory_usage(rho_core);
  bytes += MemKK::memory_usage(dF_drho_core);
  bytes += MemKK::memory_usage(dF_dfcut);
  bytes += MemKK::memory_usage(d_corerep);
  bytes += MemKK::memory_usage(dB_flatten);
  bytes += MemKK::memory_usage(fr);
  bytes += MemKK::memory_usage(dfr);
  bytes += MemKK::memory_usage(gr);
  bytes += MemKK::memory_usage(dgr);
  bytes += MemKK::memory_usage(d_values);
  bytes += MemKK::memory_usage(d_derivatives);
  bytes += MemKK::memory_usage(cr);
  bytes += MemKK::memory_usage(dcr);
  bytes += MemKK::memory_usage(d_ncount);
  bytes += MemKK::memory_usage(d_mu);
  bytes += MemKK::memory_usage(d_rhats);
  bytes += MemKK::memory_usage(d_rnorms);
  bytes += MemKK::memory_usage(d_d_min);
  bytes += MemKK::memory_usage(d_jj_min);
  bytes += MemKK::memory_usage(d_nearest);
  bytes += MemKK::memory_usage(f_ij);
  bytes += MemKK::memory_usage(d_rho_core_cutoff);
  bytes += MemKK::memory_usage(d_drho_core_cutoff);
  bytes += MemKK::memory_usage(d_E0vals);
  bytes += MemKK::memory_usage(d_ndensity);
  bytes += MemKK::memory_usage(d_npoti);
  bytes += MemKK::memory_usage(d_wpre);
  bytes += MemKK::memory_usage(d_mexp);
  bytes += MemKK::memory_usage(d_idx_ms_combs_count);
  bytes += MemKK::memory_usage(d_rank);
  bytes += MemKK::memory_usage(d_num_ms_combs);
  bytes += MemKK::memory_usage(d_idx_funcs);
  bytes += MemKK::memory_usage(d_mus);
  bytes += MemKK::memory_usage(d_ns);
  bytes += MemKK::memory_usage(d_ls);
  bytes += MemKK::memory_usage(d_ms_combs);
  bytes += MemKK::memory_usage(d_gen_cgs);
  bytes += MemKK::memory_usage(d_coeffs);
  bytes += MemKK::memory_usage(alm);
  bytes += MemKK::memory_usage(blm);
  bytes += MemKK::memory_usage(cl);
  bytes += MemKK::memory_usage(dl);
  bytes += MemKK::memory_usage(d_total_basis_size);
  bytes += MemKK::memory_usage(d_ASI);
  bytes += MemKK::memory_usage(projections);
  bytes += MemKK::memory_usage(d_gamma);

  if (k_splines_gk.h_view.data()) {
    for (int i = 0; i < nelements; i++) {
      for (int j = 0; j < nelements; j++) {
        bytes += k_splines_gk.h_view(i, j).memory_usage();
        bytes += k_splines_rnl.h_view(i, j).memory_usage();
        bytes += k_splines_hc.h_view(i, j).memory_usage();
      }
    }
  }

  return bytes;
}

/* ---------------------------------------------------------------------- */

namespace LAMMPS_NS {
template class PairPACEExtrapolationKokkos<LMPDeviceType>;
#ifdef LMP_KOKKOS_GPU
template class PairPACEExtrapolationKokkos<LMPHostType>;
#endif
}
