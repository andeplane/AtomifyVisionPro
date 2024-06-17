#import <Foundation/Foundation.h>

@interface LammpsController : NSObject {
    void *lammpsObject; // Pointer to the LAMMPS instance
    int numAtoms;       // Number of atoms in the simulation
}

// Properties
@property (nonatomic, assign) void *lammpsObject;
@property (nonatomic, assign) int numAtoms;
@property (nonatomic, strong) NSMutableArray *positions;
@property (nonatomic, strong) NSMutableArray *atomTypes;

// Methods
- (void)reset;
- (void)step;
- (void)initializeLJ;
- (void)initializeWater;
- (void)synchronize;

@end
