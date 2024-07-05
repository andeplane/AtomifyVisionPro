import SwiftUI
import RealityKit

struct ImmersiveView: View {
    @Environment(\.lammpsController) var lammps
    @State private var oxygenEntities: [ModelEntity] = []
    @State private var hydrogenEntities: [ModelEntity] = []
    @State private var atomEntities: [ModelEntity] = []
    
    var body: some View {
        RealityView { content in
            let sceneAnchor = AnchorEntity()
            content.add(sceneAnchor)
            sceneAnchor.position = [-80, -77, -100]
            startUpdatingPositions(sceneAnchor: sceneAnchor)
        }
    }

    private func startUpdatingPositions(sceneAnchor: AnchorEntity) {
        Timer.scheduledTimer(withTimeInterval: 0.001, repeats: true) { timer in
            lammps.step()
            lammps.synchronize()
            if (oxygenEntities.count == 0) {
                let oxygenMaterial = SimpleMaterial(color: .red, isMetallic: false)
                let hydrogenMaterial = SimpleMaterial(color: .white, isMetallic: false)
                for index in 0..<lammps.numAtoms {
                    if (lammps.atomTypes[index] == 1) {
                        let atom = ModelEntity(mesh: .generateSphere(radius: 0.5), materials: [oxygenMaterial])
                        atom.position = [0, 0, 0]
                        oxygenEntities.append(atom)
                        sceneAnchor.addChild(atom)
                    } else {
                        let atom = ModelEntity(mesh: .generateSphere(radius: 0.4), materials: [hydrogenMaterial])
                        atom.position = [0, 0, 0]
                        hydrogenEntities.append(atom)
                        sceneAnchor.addChild(atom)
                    }
                }
            }
            
            var hydrogenCount = 0
            var oxygenCount = 0
            for index in 0..<lammps.numAtoms {
                let position = lammps.positions[index]
                let atomType = lammps.atomTypes[index]
                if (atomType == 1) {
                    // Oxygen
                    oxygenEntities[oxygenCount].position = [position.x, position.y, position.z]
                    oxygenCount += 1
                } else {
                    hydrogenEntities[hydrogenCount].position = [position.x, position.y, position.z]
                    hydrogenCount += 1
                }
            }
        }
    }
}

#if DEBUG
struct ImmersiveView_Previews: PreviewProvider {
    static var previews: some View {
        ImmersiveView()
    }
}
#endif
