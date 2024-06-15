#import <Foundation/Foundation.h>

@interface LammpsController : NSObject {
    void *lammpsObject; // Pointer to the LAMMPS instance
    int numAtoms;       // Number of atoms in the simulation
    int atomTypes;      // Number of atom types
}

// Properties
@property (nonatomic, assign) void *lammpsObject;
@property (nonatomic, assign) int numAtoms;
@property (nonatomic, assign) int atomTypes;
@property (nonatomic, strong) NSMutableArray *positions;

// Methods
- (void)reset;
- (void)step;
- (void)initializeLJ;
- (void)synchronize;

@end
