#import "LammpsController.h"
#include "library.h" // Assuming LAMMPS provides a C API header
#include "lammps.h"
#include "atom.h"

@implementation LammpsController

// Synthesize properties
@synthesize lammpsObject;
@synthesize numAtoms;
@synthesize atomTypes;
@synthesize positions;

- (instancetype)init {
    if (self = [super init]) {
        self.lammpsObject = NULL;
        self.numAtoms = 0;
        self.atomTypes = 0;
        self.positions = nil;
        [self reset];
    }
    return self;
}

- (void)reset {
    // Deallocate any previous LAMMPS instance
    if (self.lammpsObject != NULL) {
        lammps_close(self.lammpsObject);
        lammps_mpi_finalize();
        lammps_close(self.lammpsObject);
    }
    
    int version;
    const char *lmpargv[] = { "liblammps", "-log", "none"};
    int lmpargc = sizeof(lmpargv)/sizeof(const char *);

    /* create LAMMPS instance */
    self.lammpsObject = (LAMMPS_NS::LAMMPS *)lammps_open_no_mpi(lmpargc, (char **)lmpargv, NULL);
    if (self.lammpsObject == NULL) {
    printf("LAMMPS initialization failed");
        lammps_mpi_finalize();
        return;
    }

    /* get and print numerical version code */
    version = lammps_version(self.lammpsObject);
    printf("LAMMPS Version: %d\n",version);

    // Assume some method to update numAtoms, atomTypes, positions from lammpsObject
    self.numAtoms = 0;
//    self.positions = malloc(self.numAtoms * sizeof(double) * 3); // Assuming 3D coordinates
//    lammps_gather_atoms(self.lammpsObject, "x", 1, 3, self.positions);
}

- (void) initializeLJ {
    const char *script =
    "# 3d Lennard-Jones melt\n"
    "\n"
    "variable    x index 1\n"
    "variable    y index 1\n"
    "variable    z index 1\n"
    "\n"
    "variable    xx equal 6*$x\n"
    "variable    yy equal 6*$y\n"
    "variable    zz equal 6*$z\n"
    "\n"
    "units       lj\n"
    "atom_style  atomic\n"
    "\n"
    "lattice     fcc 0.8442\n"
    "region      box block 0 ${xx} 0 ${yy} 0 ${zz}\n"
    "create_box  1 box\n"
    "create_atoms 1 box\n"
    "mass        1 1.0\n"
    "\n"
    "velocity    all create 1.44 87287 loop geom\n"
    "\n"
    "pair_style  lj/cut 2.5\n"
    "pair_coeff  1 1 1.0 1.0 2.5\n"
    "\n"
    "neighbor    0.3 bin\n"
    "neigh_modify delay 0 every 20 check no\n"
    "\n"
    "fix         1 all nve\n"
    "\n";
    lammps_commands_string(self.lammpsObject, script);
    [self synchronize];
    
}

- (void)step {
    // Execute a single timestep
    lammps_command(self.lammpsObject, "run 1 pre no post no");
}

- (void)synchronize {
    if (!self.lammpsObject) {
        NSLog(@"lammpsObject is null");
        return;
    } else {
        LAMMPS_NS::LAMMPS *lammps = (LAMMPS_NS::LAMMPS *)self.lammpsObject;
        LAMMPS_NS::Atom *atom = lammps->atom;
        self.numAtoms = atom->natoms;
        NSLog(@"Synchronized with %d atoms ", self.numAtoms);
        
        if (self.positions && self.positions.count != self.numAtoms) {
            // Need to reallocate
            self.positions = nil;
        }
        if (!self.positions) {
            self.positions = [[NSMutableArray alloc] initWithCapacity:3 * self.numAtoms]; // x, y, z
        }
        double **lammpsPositions = atom->x;
        for (int i = 0; i < self.numAtoms; i++) {
            double x = atom->x[i][0];
            double y = atom->x[i][1];
            double z = atom->x[i][2];
            self.positions[3*i+0] = @(x);
            self.positions[3*i+1] = @(y);
            self.positions[3*i+2] = @(z);
        }
        
    }
}


@end
