/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Axel Kohlmeyer (Temple U)
------------------------------------------------------------------------- */

#include "pair_lepton_sphere.h"

#include "atom.h"
#include "comm.h"
#include "error.h"
#include "force.h"
#include "neigh_list.h"
#include "neighbor.h"

#include "Lepton.h"
#include "lepton_utils.h"

#include <array>
#include <cmath>
#include <exception>

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

void PairLeptonSphere::compute(int eflag, int vflag)
{
  ev_init(eflag, vflag);
  if (evflag) {
    if (eflag) {
      if (force->newton_pair)
        eval<1, 1, 1>();
      else
        eval<1, 1, 0>();
    } else {
      if (force->newton_pair)
        eval<1, 0, 1>();
      else
        eval<1, 0, 0>();
    }
  } else {
    if (force->newton_pair)
      eval<0, 0, 1>();
    else
      eval<0, 0, 0>();
  }
  if (vflag_fdotr) virial_fdotr_compute();
}

/* ---------------------------------------------------------------------- */

template <int EVFLAG, int EFLAG, int NEWTON_PAIR> void PairLeptonSphere::eval()
{
  const double *const *const x = atom->x;
  double *const *const f = atom->f;
  const double *const radius = atom->radius;
  const int *const type = atom->type;
  const int nlocal = atom->nlocal;
  const double *const special_lj = force->special_lj;

  const int inum = list->inum;
  const int *const ilist = list->ilist;
  const int *const numneigh = list->numneigh;
  const int *const *const firstneigh = list->firstneigh;
  double fxtmp, fytmp, fztmp;

  std::vector<Lepton::CompiledExpression> pairforce;
  std::vector<Lepton::CompiledExpression> pairpot;
  std::vector<std::array<bool, 3>> has_ref;
  try {
    for (const auto &expr : expressions) {
      auto parsed = Lepton::Parser::parse(LeptonUtils::substitute(expr, lmp), functions);
      pairforce.emplace_back(parsed.differentiate("r").createCompiledExpression());
      has_ref.push_back({true, true, true});
      try {
        pairforce.back().getVariableReference("r");
      } catch (Lepton::Exception &) {
        has_ref.back()[0] = false;
      }
      if (EFLAG) pairpot.emplace_back(parsed.createCompiledExpression());

      // check if there are references to radii

      try {
        pairforce.back().getVariableReference("radi");
      } catch (Lepton::Exception &) {
        has_ref.back()[1] = false;
      }
      try {
        pairforce.back().getVariableReference("radj");
      } catch (Lepton::Exception &) {
        has_ref.back()[2] = false;
      }
    }
  } catch (std::exception &e) {
    error->all(FLERR, e.what());
  }

  // loop over neighbors of my atoms

  for (int ii = 0; ii < inum; ii++) {
    const int i = ilist[ii];
    const double xtmp = x[i][0];
    const double ytmp = x[i][1];
    const double ztmp = x[i][2];
    const int itype = type[i];
    const int *jlist = firstneigh[i];
    const int jnum = numneigh[i];
    fxtmp = fytmp = fztmp = 0.0;

    for (int jj = 0; jj < jnum; jj++) {
      int j = jlist[jj];
      const double factor_lj = special_lj[sbmask(j)];
      j &= NEIGHMASK;
      const int jtype = type[j];

      const double delx = xtmp - x[j][0];
      const double dely = ytmp - x[j][1];
      const double delz = ztmp - x[j][2];
      const double rsq = delx * delx + dely * dely + delz * delz;

      if (rsq < cutsq[itype][jtype]) {
        const double r = sqrt(rsq);
        const int idx = type2expression[itype][jtype];
        if (has_ref[idx][0]) pairforce[idx].getVariableReference("r") = r;
        if (has_ref[idx][1]) pairforce[idx].getVariableReference("radi") = radius[i];
        if (has_ref[idx][2]) pairforce[idx].getVariableReference("radj") = radius[j];
        const double fpair = -pairforce[idx].evaluate() / r * factor_lj;

        fxtmp += delx * fpair;
        fytmp += dely * fpair;
        fztmp += delz * fpair;
        if (NEWTON_PAIR || (j < nlocal)) {
          f[j][0] -= delx * fpair;
          f[j][1] -= dely * fpair;
          f[j][2] -= delz * fpair;
        }

        double evdwl = 0.0;
        if (EFLAG) {
          try {
            pairpot[idx].getVariableReference("r") = r;
          } catch (Lepton::Exception &) {
            ;    // ignore -> constant potential
          }
          if (has_ref[idx][1]) pairpot[idx].getVariableReference("radi") = radius[i];
          if (has_ref[idx][2]) pairpot[idx].getVariableReference("radj") = radius[j];

          evdwl = pairpot[idx].evaluate();
          evdwl *= factor_lj;
        }

        if (EVFLAG) ev_tally(i, j, nlocal, NEWTON_PAIR, evdwl, 0.0, fpair, delx, dely, delz);
      }
    }
    f[i][0] += fxtmp;
    f[i][1] += fytmp;
    f[i][2] += fztmp;
  }
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairLeptonSphere::settings(int narg, char **arg)
{
  if (narg != 1)
    error->all(FLERR, "Incorrect number of arguments for pair_style lepton/sphere command");
  cut_global = utils::numeric(FLERR, arg[0], false, lmp);
}

/* ---------------------------------------------------------------------- */

void PairLeptonSphere::init_style()
{
  if (!atom->radius_flag)
    error->all(FLERR, "Pair style lepton/sphere requires atom attribute radius");
  if (offset_flag) error->all(FLERR, "Pair style lepton/sphere does not suport pair_modify shift");
  neighbor->add_request(this);
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLeptonSphere::write_restart_settings(FILE *fp)
{
  fwrite(&cut_global, sizeof(double), 1, fp);
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLeptonSphere::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) { utils::sfread(FLERR, &cut_global, sizeof(double), 1, fp, nullptr, error); }
  MPI_Bcast(&cut_global, 1, MPI_DOUBLE, 0, world);
}

/* ---------------------------------------------------------------------- */

double PairLeptonSphere::single(int i, int j, int itype, int jtype, double rsq,
                                double /* factor_coul */, double factor_lj, double &fforce)
{
  const auto &expr = expressions[type2expression[itype][jtype]];
  auto parsed = Lepton::Parser::parse(LeptonUtils::substitute(expr, lmp), functions);
  auto pairpot = parsed.createCompiledExpression();
  auto pairforce = parsed.differentiate("r").createCompiledExpression();

  const double r = sqrt(rsq);
  try {
    pairpot.getVariableReference("r") = r;
    pairforce.getVariableReference("r") = r;
  } catch (Lepton::Exception &) {
    ;    // ignore -> constant potential or force
  }
  try {
    pairpot.getVariableReference("radi") = atom->radius[i];
    pairforce.getVariableReference("radi") = atom->radius[i];
  } catch (Lepton::Exception &) {
    ;    // ignore
  }
  try {
    pairpot.getVariableReference("radj") = atom->radius[j];
    pairforce.getVariableReference("radj") = atom->radius[j];
  } catch (Lepton::Exception &) {
    ;    // ignore
  }

  fforce = -pairforce.evaluate() / r * factor_lj;
  return pairpot.evaluate() * factor_lj;
}
