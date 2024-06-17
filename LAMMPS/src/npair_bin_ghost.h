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

#ifdef NPAIR_CLASS
// clang-format off
typedef NPairBinGhost<0> NPairFullBinGhost;
NPairStyle(full/bin/ghost,
           NPairFullBinGhost,
           NP_FULL | NP_BIN | NP_NEWTON | NP_NEWTOFF | NP_GHOST | NP_ORTHO | NP_TRI);

typedef NPairBinGhost<1> NPairHalfBinGhostNewtoff;
NPairStyle(half/bin/ghost/newtoff,
           NPairHalfBinGhostNewtoff,
           NP_HALF | NP_BIN | NP_NEWTOFF | NP_GHOST | NP_ORTHO | NP_TRI);
// clang-format on
#else

#ifndef LMP_NPAIR_BIN_GHOST_H
#define LMP_NPAIR_BIN_GHOST_H

#include "npair.h"

namespace LAMMPS_NS {

template<int HALF>
class NPairBinGhost : public NPair {
 public:
  NPairBinGhost(class LAMMPS *);
  void build(class NeighList *) override;
};

}    // namespace LAMMPS_NS

#endif
#endif
