#include "Layout.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void fill(SDL_Renderer* renderer, PiLayout::Rect r, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_Rect rect{r.x, r.y, r.w, r.h};
  SDL_RenderFillRect(renderer, &rect);
}

void label(SDL_Renderer* renderer, const std::string& text, PiLayout::Rect area,
           int fontSize, SDL_Color color, const std::string& fontPath) {
  if (area.w <= 0 || area.h <= 0) return;
  TTF_Font* font = TTF_OpenFont(fontPath.c_str(), fontSize);
  if (!font) throw std::runtime_error(TTF_GetError());
  SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
  TTF_CloseFont(font);
  if (!surface) throw std::runtime_error(TTF_GetError());
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
  if (!texture) {
    SDL_FreeSurface(surface);
    throw std::runtime_error(SDL_GetError());
  }
  const double scale = std::min({1.0, double(area.w) / surface->w,
                                  double(area.h) / surface->h});
  SDL_Rect target{area.x, area.y, std::max(1, int(surface->w * scale)),
                  std::max(1, int(surface->h * scale))};
  SDL_RenderCopy(renderer, texture, nullptr, &target);
  SDL_DestroyTexture(texture);
  SDL_FreeSurface(surface);
}

void draw(SDL_Renderer* renderer, int width, int height,
          const std::string& fontPath) {
  const auto frame = PiLayout::makeFrame(width, height, 4, 3);
  fill(renderer, {0, 0, width, height}, {16, 32, 49, 255});
  fill(renderer, frame.safe, {244, 247, 250, 255});
  fill(renderer, frame.header, {18, 65, 94, 255});
  const int pad = std::max(6, frame.safe.x / 2);
  label(renderer, "DISCOVER AROUND ME - DEVELOPMENT FIXTURE",
        {frame.header.x + pad, frame.header.y + pad,
         frame.header.w - 2 * pad, frame.header.h - 2 * pad},
        std::clamp(height / 28, 14, 36), {255, 255, 255, 255}, fontPath);

  // A 4:3 graphic fixture centered with contain. The server asset fetch and
  // actual decoded image are later stages; this color block proves geometry.
  fill(renderer, frame.graphic, {208, 227, 235, 255});
  const int inset = std::max(6, frame.graphic.w / 25);
  label(renderer, "FORECAST", {frame.graphic.x + inset, frame.graphic.y + inset,
                                frame.graphic.w - 2 * inset, frame.graphic.h / 5},
        std::clamp(height / 20, 18, 50), {18, 65, 94, 255}, fontPath);
  label(renderer, "72 F  |  Sample content",
        {frame.graphic.x + inset, frame.graphic.y + frame.graphic.h / 3,
         frame.graphic.w - 2 * inset, frame.graphic.h / 3},
        std::clamp(height / 13, 22, 78), {18, 65, 94, 255}, fontPath);

  fill(renderer, frame.footer, {224, 231, 235, 255});
  label(renderer, "Fixture only - no cloud data or device enrollment",
        {frame.footer.x + pad, frame.footer.y + pad,
         frame.footer.w - 2 * pad, frame.footer.h - 2 * pad},
        std::clamp(height / 34, 12, 28), {18, 65, 94, 255}, fontPath);
  SDL_RenderPresent(renderer);
}

void save(SDL_Renderer* renderer, int width, int height,
          const std::string& path) {
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
      0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surface) throw std::runtime_error(SDL_GetError());
  if (SDL_RenderReadPixels(renderer, nullptr, surface->format->format,
                           surface->pixels, surface->pitch) != 0 ||
      SDL_SaveBMP(surface, path.c_str()) != 0) {
    std::string error = SDL_GetError();
    SDL_FreeSurface(surface);
    throw std::runtime_error(error);
  }
  SDL_FreeSurface(surface);
}

}  // namespace

int main(int argc, char** argv) {
  int width = 1280, height = 720;
  std::string screenshot;
  std::string font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if ((arg == "--width" || arg == "--height" || arg == "--once" ||
           arg == "--font") && i + 1 < argc) {
        const std::string value = argv[++i];
        if (arg == "--width") width = std::stoi(value);
        else if (arg == "--height") height = std::stoi(value);
        else if (arg == "--font") font = value;
        else screenshot = value;
      } else {
        throw std::runtime_error("usage: pi-card [--width N --height N] [--font PATH] [--once output.bmp]");
      }
    }
    if (width < 160 || height < 120 || width > 3840 || height > 2160)
      throw std::runtime_error("viewport outside 160x120..3840x2160");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    if (TTF_Init() != 0) throw std::runtime_error(TTF_GetError());
    SDL_Window* window = SDL_CreateWindow("Discover Around Me Pi fixture",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
        screenshot.empty() ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_HIDDEN);
    if (!window) throw std::runtime_error(SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    SDL_GetRendererOutputSize(renderer, &width, &height);
    draw(renderer, width, height, font);
    if (!screenshot.empty()) {
      save(renderer, width, height, screenshot);
      std::cout << "fixture frame " << width << 'x' << height << " -> "
                << screenshot << '\n';
    } else {
      bool running = true;
      while (running) {
        SDL_Event event;
        while (SDL_WaitEvent(&event)) {
          if (event.type == SDL_QUIT ||
              (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
            running = false;
          if (event.type == SDL_WINDOWEVENT &&
              event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            SDL_GetRendererOutputSize(renderer, &width, &height);
            draw(renderer, width, height, font);
          }
          break;
        }
      }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
  } catch (const std::exception& e) {
    std::cerr << "pi-card: " << e.what() << '\n';
    return 1;
  }
}
