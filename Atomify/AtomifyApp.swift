//
//  AtomifyApp.swift
//  Atomify
//
//  Created by Anders Hafreager on 15/06/2024.
//

import SwiftUI

@main
struct AtomifyApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }

        ImmersiveSpace(id: "ImmersiveSpace") {
            ImmersiveView()
        }
    }
}
