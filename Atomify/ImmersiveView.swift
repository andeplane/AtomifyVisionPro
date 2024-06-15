import SwiftUI
import RealityKit

struct ImmersiveView: View {
    @StateObject private var lammps = LammpsWrapper()
    @State private var atomEntities: [ModelEntity] = []
    
    var body: some View {
        RealityView { content in
            let sceneAnchor = AnchorEntity()
            content.add(sceneAnchor)
            sceneAnchor.position = [-5, -5, -10]
            startUpdatingPositions(sceneAnchor: sceneAnchor)
        }
    }

    private func startUpdatingPositions(sceneAnchor: AnchorEntity) {
        Timer.scheduledTimer(withTimeInterval: 0.001, repeats: true) { timer in
            lammps.step()
            lammps.synchronize()
            
            print("Count of entities \(atomEntities.count) and num atoms \(lammps.numAtoms)")
            if (atomEntities.count != lammps.numAtoms) {
                print("Did not have renderables. Creating meshes...")
                for _ in 0..<lammps.numAtoms {
                    let atom = ModelEntity(mesh: .generateSphere(radius: 0.2), materials: [SimpleMaterial(color: .red, isMetallic: false)])
                    atom.position = [0, 0, 0]
                    atomEntities.append(atom)
                    sceneAnchor.addChild(atom)
                }
            }
            
            for (index, atom) in atomEntities.enumerated() {
                let position = lammps.positions[index]
                atom.position = [position.x, position.y, position.z]
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
