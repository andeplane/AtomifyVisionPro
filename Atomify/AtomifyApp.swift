//
//  AtomifyApp.swift
//  Atomify
//
//  Created by Anders Hafreager on 15/06/2024.
//

import SwiftUI
import Foundation
// import LAMMPS
@main
struct AtomifyApp: App {
    @State private var selectedSimulation: Simulation? = nil
    @StateObject private var lammpsController = LammpsWrapper()
    @State private var atomStyle: ImmersionStyle = .full
    init() {
        // Example usage:
        do {
            if let simulations = try Simulation.loadSimulations() {
                for simulation in simulations {
                    print("Simulation ID: \(simulation.id)")
                    print("Title: \(simulation.title)")
                    print("Description: \(simulation.description)")
                    print("Analysis Description: \(simulation.analysisDescription ?? "N/A")")
                    print("Input Script: \(simulation.inputScript)")
                    print("Keywords: \(simulation.keywords)")
                    print("Files: \(simulation.files.map { $0.fileName })")
                    if let image = simulation.imageFile {
                        print("Image loaded successfully")
                    }
                }
            }
        } catch {
            print("An error occurred: \(error.localizedDescription)")
        }

    }
    var body: some Scene {
        WindowGroup {
            ContentView(selectedSimulation: $selectedSimulation)
            .onChange(of: selectedSimulation) { newSelectedSimulation in
                handleNewSimulationChange(newSelectedSimulation: newSelectedSimulation)
            }
        }

        ImmersiveSpace(id: "ImmersiveSpace") {
            ImmersiveView()
        }
        .immersionStyle(selection: $atomStyle, in: .full)
        .environment(\.lammpsController, lammpsController) // Pass the selected simulation
    }
    
    private func handleNewSimulationChange(newSelectedSimulation: Simulation?) {
        print("State changed to: \(String(describing: newSelectedSimulation?.title))")
        // Add any additional actions you want to perform on state change here
    }
}
