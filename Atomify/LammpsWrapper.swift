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
    @Published var numAtoms: Int32 = 0
    private var controller: LammpsController
    
    init() {
        controller = LammpsController()
        controller.initializeLJ()
        numAtoms = controller.numAtoms
        updatePositions()
        print("Initialized LammpsWrapper with numAtoms: \(numAtoms)")

    }
    
    func step() {
        controller.step()
    }
    
    func synchronize() {
        controller.synchronize()
        numAtoms = controller.numAtoms
        updatePositions()
    }
    
    private func updatePositions() {
        if let nsArray = controller.positions as? [Double] {
            positions = stride(from: 0, to: nsArray.count, by: 3).map {
                SIMD3<Float>(Float(nsArray[$0]), Float(nsArray[$0 + 1]), Float(nsArray[$0 + 2]))
            }
        }
    }
}
