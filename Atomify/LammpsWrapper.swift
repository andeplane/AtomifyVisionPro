import Foundation
import Combine
import simd

extension NSValue {
    func simdValue() -> SIMD3<Float> {
        var simdValue = SIMD3<Float>(0, 0, 0)
        getValue(&simdValue)
        return simdValue
    }
}

class LammpsWrapper: ObservableObject {
    @Published var positions: [SIMD3<Float>] = []
    @Published var numAtoms: Int = 0
    @Published var atomTypes: [Int] = []  // Array of atom types as integers
    private var controller: LammpsController
    
    init() {
        controller = LammpsController()
        controller.initializeWater()
        numAtoms = Int(controller.numAtoms)
        updatePositions()
        updateAtomTypes()
        print("Initialized LammpsWrapper with numAtoms: \(numAtoms)")
    }
    
    func step() {
        controller.step()
    }
    
    func reset() {
        controller.reset()
        controller.initializeLJ()
        updateAtomTypes()
    }
    
    func synchronize() {
        controller.synchronize()
        numAtoms = Int(controller.numAtoms)
        updatePositions()
        updateAtomTypes()
    }
    
    private func updatePositions() {
        if let nsArray = controller.positions as? [Double] {
            positions = stride(from: 0, to: nsArray.count, by: 3).map {
                SIMD3<Float>(Float(nsArray[$0]), Float(nsArray[$0 + 1]), Float(nsArray[$0 + 2]))
            }
        }
    }
    
    private func updateAtomTypes() {
        if let atomTypesFromController = controller.atomTypes as? [Int] {
            atomTypes = atomTypesFromController
        }
    }
}
