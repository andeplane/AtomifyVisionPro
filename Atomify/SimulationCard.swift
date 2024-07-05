import SwiftUI

struct SimulationCard: View {
    @ObservedObject var simulation: Simulation
    var action: () -> Void // Define the action to be performed on tap
    var cornerRadius: CGFloat = 10.0 // Add a parameter for corner radius with a default value

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading) {
                if let image = simulation.imageFile {
                    Image(uiImage: image)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .cornerRadius(cornerRadius)
                } else {
                    Rectangle()
                        .fill(Color.gray)
                        .cornerRadius(cornerRadius)
                        .frame(maxHeight: 100)
                }
                Text(simulation.title)
                    .font(.headline)
                    .padding(.top, 8)
                Text(simulation.description)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .padding(.top, 4)
            }
            .padding()
            .background(Color(.systemBackground))
            .cornerRadius(cornerRadius)
            .shadow(radius: 5)
        }
        .buttonBorderShape(.roundedRectangle(radius: 20))
        .frame(minWidth: 400, maxWidth: 400) // Set a minimum width for the cards

    }
}

struct SimulationCard_Previews: PreviewProvider {
    static var previews: some View {
        let simulation = Simulation(id: "watervapor", title: "Water vapor", description: "Low density water gas using the vashishta potential.", analysisDescription: nil, imageUrl: "simulations/water/vapor/vapor.png", inputScript: "vapor.in", keywords: ["vashishta", "water"], files: [], imageFile: UIImage(named: "placeholder"))
        
        SimulationCard(simulation: simulation, action: {
            print("Simulation tapped")
        }, cornerRadius: 5.0) // Adjust the corner radius for the preview
            .previewLayout(.sizeThatFits)
    }
}
