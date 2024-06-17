// clang-format off
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

#include "fix_enforce2d.h"

#include "atom.h"
#include "domain.h"
#include "error.h"
#include "respa.h"
#include "update.h"

using namespace LAMMPS_NS;
using namespace FixConst;

/* ---------------------------------------------------------------------- */

FixEnforce2D::FixEnforce2D(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  if (narg != 3) error->all(FLERR,"Illegal fix enforce2d command");
}

/* ---------------------------------------------------------------------- */

FixEnforce2D::~FixEnforce2D()
{
  if (copymode) return;
}

/* ---------------------------------------------------------------------- */

int FixEnforce2D::setmask()
{
  int mask = 0;
  mask |= POST_FORCE;
  mask |= POST_FORCE_RESPA;
  mask |= MIN_POST_FORCE;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixEnforce2D::init()
{
  if (domain->dimension == 3)
    error->all(FLERR,"Cannot use fix enforce2d with 3d simulation");
}

/* ---------------------------------------------------------------------- */

void FixEnforce2D::setup(int vflag)
{
  if (utils::strmatch(update->integrate_style,"^verlet"))
    post_force(vflag);
  else {
    int nlevels_respa = (dynamic_cast<Respa *>(update->integrate))->nlevels;
    for (int ilevel = 0; ilevel < nlevels_respa; ilevel++) {
      (dynamic_cast<Respa *>(update->integrate))->copy_flevel_f(ilevel);
      post_force_respa(vflag,ilevel,0);
      (dynamic_cast<Respa *>(update->integrate))->copy_f_flevel(ilevel);
    }
  }
}

/* ---------------------------------------------------------------------- */

void FixEnforce2D::min_setup(int vflag)
{
  post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixEnforce2D::post_force(int /*vflag*/)
{
  double **v = atom->v;
  double **f = atom->f;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  if (igroup == atom->firstgroup) nlocal = atom->nfirst;

  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      v[i][2] = 0.0;
      f[i][2] = 0.0;
    }

  // for systems with omega/angmom/torque, zero x and y components

  if (atom->omega_flag) {
    double **omega = atom->omega;
    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        omega[i][0] = 0.0;
        omega[i][1] = 0.0;
      }
  }

  if (atom->angmom_flag) {
    double **angmom = atom->angmom;
    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        angmom[i][0] = 0.0;
        angmom[i][1] = 0.0;
      }
  }

  if (atom->torque_flag) {
    double **torque = atom->torque;
    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        torque[i][0] = 0.0;
        torque[i][1] = 0.0;
      }
  }
}

/* ---------------------------------------------------------------------- */

void FixEnforce2D::post_force_respa(int vflag, int /*ilevel*/, int /*iloop*/)
{
  post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixEnforce2D::min_post_force(int vflag)
{
  post_force(vflag);
}
