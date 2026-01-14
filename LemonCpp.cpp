#define LEMONCPP_AND_VERSION "LemonCpp 2.0.7 pre"
#include "textbox.hpp"
int main() {
    // FreeConsole();
    sf::RenderWindow window(sf::VideoMode({1000,700}),LEMONCPP_AND_VERSION);

    sf::Image icon;
    if (icon.loadFromFile("LemonCpp.png")) {
        window.setIcon({icon.getSize().x, icon.getSize().y}, icon.getPixelsPtr());
    }

    window.setFramerateLimit(30);
    
    TextBox editor({10,10},{980,680}, &window);

    while (window.isOpen()) {
        while (auto event = window.pollEvent()) {
            // let editor handle events first (it will prompt on close and can prevent closing)
            editor.handleEvent(*event, window);
            // do not close the window here; editor will call hostWindow->close() after prompting
        }
        editor.update();
        window.clear(sf::Color::White);
        editor.draw(window);
        window.display();
    }
}
