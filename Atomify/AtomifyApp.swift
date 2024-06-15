//
//  AtomifyApp.swift
//  Atomify
//
//  Created by Anders Hafreager on 15/06/2024.
//

import SwiftUI
// import LAMMPS
@main
struct AtomifyApp: App {
    init() {
//        LAMMPSBridge.runLAMMPS()
    }
    var body: some Scene {
        WindowGroup {
            ContentView()
        }

        ImmersiveSpace(id: "ImmersiveSpace") {
            ImmersiveView()
        }
    }
}
