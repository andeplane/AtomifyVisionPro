import SwiftUI
import RealityKit
import RealityKitContent

struct ContentView: View {
    @State private var showImmersiveSpace = false
    @State private var immersiveSpaceIsShown = false
    @State private var simulations: [Simulation] = []
    @Binding var selectedSimulation: Simulation?


    let columns: [GridItem] = [
        GridItem(.flexible(minimum: 200)),
        GridItem(.flexible(minimum: 200)),
        GridItem(.flexible(minimum: 200))
    ]

    @Environment(\.openImmersiveSpace) var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) var dismissImmersiveSpace

    var body: some View {
        ScrollView {
            VStack(spacing: 20) {
                Text("Welcome to Atomify")
                    .font(.largeTitle)
                    .fontWeight(.bold)
                    .padding(.top, 20)

                Text("Please select a simulation to get started.")
                    .font(.title2)
                    .foregroundColor(.secondary)

                LazyVGrid(columns: columns, spacing: 20) {
                    ForEach(simulations) { simulation in
                        SimulationCard(simulation: simulation, action: {
                            selectedSimulation = simulation
                            showImmersiveSpace = true
                        }, cornerRadius: 5.0)
                        .aspectRatio(3/2, contentMode: .fit)
                        .frame(minWidth: 450, minHeight: 250)
                    }
                }
                .padding()
            }
            .frame(maxWidth: .infinity)
        }
        .onAppear {
            loadSimulations()
        }
        .padding()
        .onChange(of: showImmersiveSpace) { _, newValue in
            Task {
                if newValue {
                    switch await openImmersiveSpace(id: "ImmersiveSpace") {
                    case .opened:
                        immersiveSpaceIsShown = true
                    case .error, .userCancelled:
                        fallthrough
                    @unknown default:
                        immersiveSpaceIsShown = false
                        showImmersiveSpace = false
                    }
                } else if immersiveSpaceIsShown {
                    await dismissImmersiveSpace()
                    immersiveSpaceIsShown = false
                }
            }
        }
    }

    private func loadSimulations() {
        do {
            if let loadedSimulations = try Simulation.loadSimulations() {
                self.simulations = loadedSimulations
            }
        } catch {
            print("Failed to load simulations: \(error.localizedDescription)")
        }
    }
}

#Preview(windowStyle: .automatic) {
    @State var previewSelectedSimulation: Simulation? = nil

    ContentView(selectedSimulation: $previewSelectedSimulation)
}
