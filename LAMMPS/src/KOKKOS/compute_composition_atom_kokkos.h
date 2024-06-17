/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef COMPUTE_CLASS
// clang-format off
ComputeStyle(composition/atom/kk,ComputeCompositionAtomKokkos<LMPDeviceType>);
ComputeStyle(composition/atom/kk/device,ComputeCompositionAtomKokkos<LMPDeviceType>);
ComputeStyle(composition/atom/kk/host,ComputeCompositionAtomKokkos<LMPHostType>);
// clang-format on

#else

#ifndef LMP_COMPUTE_COMPOSITION_ATOM_KOKKOS_H
#define LMP_COMPUTE_COMPOSITION_ATOM_KOKKOS_H

#include "compute_composition_atom.h"
#include "kokkos_type.h"

namespace LAMMPS_NS {

// clang-format off
struct TagComputeCompositionAtom {};
// clang-format on

template <class DeviceType> class ComputeCompositionAtomKokkos : public ComputeCompositionAtom {
 public:
  typedef DeviceType device_type;
  typedef ArrayTypes<DeviceType> AT;

  ComputeCompositionAtomKokkos(class LAMMPS *, int, char **);
  ~ComputeCompositionAtomKokkos() override;
  void init() override;
  void compute_peratom() override;

  KOKKOS_INLINE_FUNCTION
  void operator()(TagComputeCompositionAtom, const int &) const;

 private:

  typename AT::t_x_array x;
  typename ArrayTypes<DeviceType>::t_int_1d type;
  typename ArrayTypes<DeviceType>::t_int_1d mask;

  typename AT::t_neighbors_2d d_neighbors;
  typename AT::t_int_1d d_ilist;
  typename AT::t_int_1d d_numneigh;
  DAT::tdual_float_2d k_result;
  typename AT::t_float_2d d_result;
};

}    // namespace LAMMPS_NS

#endif
#endif
