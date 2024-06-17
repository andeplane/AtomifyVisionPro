#import "LammpsController.h"
#include "src/library.h" // Assuming LAMMPS provides a C API header
#include "src/lammps.h"
#include "src/atom.h"
#include "src/domain.h"

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
        self.atomTypes = nil;
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
    const char *lmpargv[] = { "liblammps", "-log", "none", "-sc", "none"};
//    const char *lmpargv[] = { "liblammps", "-log", "none"};
    int lmpargc = sizeof(lmpargv)/sizeof(const char *);

    /* create LAMMPS instance */
    self.lammpsObject = (LAMMPS_NS::LAMMPS *)lammps_open_no_mpi(lmpargc, (char **)lmpargv, NULL);
    if (self.lammpsObject == NULL) {
    printf("LAMMPS initialization failed");
        lammps_mpi_finalize();
        return;
    }

    version = lammps_version(self.lammpsObject);
    printf("LAMMPS Version: %d\n",version);

    self.numAtoms = 0;
}

- (void) initializeWater {
    // Get the main bundle path
    NSString *bundlePath = [[NSBundle mainBundle] bundlePath];
    NSString *simulationFolderPath =[bundlePath stringByAppendingString:@"/simulations/water/vapor"];
    // Append the specific path to the bundle path
    NSString *fullPath = [simulationFolderPath stringByAppendingString:@"/vapor.in"];

    // Convert the NSString to a C-style string
    const char *cFullPath = [fullPath UTF8String];
    NSLog(@"Simulation path: %@", simulationFolderPath);
    
    // Try to change the current directory
    [[NSFileManager defaultManager] changeCurrentDirectoryPath:simulationFolderPath];
    
    lammps_file(self.lammpsObject, cFullPath);
    lammps_command(self.lammpsObject, "timestep 0.0002");
    
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
        LAMMPS_NS::Domain *domain = lammps->domain;
        
        self.numAtoms = atom->natoms;
        
        if (self.positions && self.positions.count != self.numAtoms) {
            // Need to reallocate
            self.positions = nil;
        }
        
        if (self.atomTypes && self.atomTypes.count != self.numAtoms) {
            // Need to reallocate
            self.atomTypes = nil;
        }
        
        if (!self.positions) {
            self.positions = [[NSMutableArray alloc] initWithCapacity:3 * self.numAtoms]; // x, y, z
        }
        if (!self.atomTypes) {
            self.atomTypes = [[NSMutableArray alloc] initWithCapacity: self.numAtoms];
        }
        
        for (int i = 0; i < self.numAtoms; i++) {
            double position[3];
            position[0] = atom->x[i][0];
            position[1] = atom->x[i][1];
            position[2] = atom->x[i][2];
            domain->remap(position); // remap into system boundaries with PBC
            
            self.positions[3*i+0] = @(position[0]);
            self.positions[3*i+1] = @(position[1]);
            self.positions[3*i+2] = @(position[2]);
            self.atomTypes[i] = @(atom->type[i]);
        }
        
    }
}


@end
