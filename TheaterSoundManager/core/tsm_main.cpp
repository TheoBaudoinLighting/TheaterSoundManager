#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>

#include <SDL.h>
#include <SDL_opengl.h>

// ImGui
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

// File dialog
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

// FMOD
#include <fmod.hpp>
#include <fmod_errors.h>

const char* glsl_version = "#version 130";

namespace fs = std::filesystem;

// Structure pour stocker les informations des musiques
struct MusicTrack {
    std::string filepath;
    std::string filename;
    FMOD::Sound* sound;
    std::vector<float> waveform; // Données de la forme d'onde
};

std::vector<MusicTrack> musicTracks;
std::mutex musicTracksMutex;

// Fonction pour ouvrir le sélecteur de fichiers natif de Windows
std::vector<std::string> OpenFileDialog(const char* filter = "Audio Files\0*.mp3;*.wav;*.ogg;*.flac\0All Files\0*.*\0") {
    std::vector<std::string> filenames;

    OPENFILENAMEA ofn;       // structure pour l'API
    CHAR szFile[8192] = { 0 }; // buffer pour le nom de fichier

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL; // Pas de fenêtre propriétaire
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    ofn.lpstrDefExt = "mp3";

    if (GetOpenFileNameA(&ofn)) {
        char* p = szFile;
        std::string directory = p;
        p += directory.length() + 1;

        if (*p == '\0') {
            // Un seul fichier sélectionné
            filenames.push_back(directory);
        }
        else {
            // Plusieurs fichiers sélectionnés
            while (*p) {
                std::string filename = p;
                p += filename.length() + 1;
                filenames.push_back(directory + "\\" + filename);
            }
        }
    }

    return filenames;
}

bool init_sdl(SDL_Window** window, SDL_GLContext* gl_context, const char* title, int width, int height)
{
    // Configuration du contexte OpenGL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
    {
        std::cerr << "Erreur SDL_Init: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    *window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, window_flags);

    if (*window == nullptr)
    {
        std::cerr << "Erreur SDL_CreateWindow: " << SDL_GetError() << std::endl;
        return false;
    }

    *gl_context = SDL_GL_CreateContext(*window);
    SDL_GL_MakeCurrent(*window, *gl_context);
    SDL_GL_SetSwapInterval(1);

    if (*gl_context == nullptr)
    {
        std::cerr << "Erreur SDL_GL_CreateContext: " << SDL_GetError() << std::endl;
        return false;
    }

    return true;
}

void cleanup_sdl(SDL_Window* window, SDL_GLContext gl_context)
{
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

// Fonction pour vérifier les erreurs FMOD
void ERRCHECK(FMOD_RESULT result)
{
    if (result != FMOD_OK)
    {
        std::cerr << "FMOD error (" << result << "): " << FMOD_ErrorString(result) << std::endl;
    }
}

// Fonction pour générer la forme d'onde de manière asynchrone
void GenerateWaveform(FMOD::System* fmodSystem, MusicTrack& track, std::mutex& mutex)
{
    std::lock_guard<std::mutex> lock(mutex);
    FMOD::Sound* sound = nullptr;
    FMOD_RESULT result;

    // Ouvrir le son sans streaming pour un accès aléatoire plus rapide
    result = fmodSystem->createSound(track.filepath.c_str(), FMOD_DEFAULT | FMOD_ACCURATETIME, nullptr, &sound);
    if (result != FMOD_OK)
    {
        std::cerr << "Erreur FMOD::createSound: " << FMOD_ErrorString(result) << std::endl;
        return;
    }

    unsigned int length = 0;
    result = sound->getLength(&length, FMOD_TIMEUNIT_PCM);
    ERRCHECK(result);

    int numChannels = 0;
    result = sound->getFormat(nullptr, nullptr, &numChannels, nullptr);
    ERRCHECK(result);

    int numSamples = 1024; // Nombre de points pour le tracé
    track.waveform.resize(numSamples);

    unsigned int totalSamples = length;
    unsigned int samplesPerPoint = totalSamples / numSamples;

    std::vector<short> sampleBuffer(numChannels);

    for (int i = 0; i < numSamples; ++i)
    {
        unsigned int position = i * samplesPerPoint;
        if (position >= totalSamples)
            position = totalSamples - 1;

        // Définir la position de lecture
        result = sound->seekData(position * numChannels * sizeof(short));
        ERRCHECK(result);

        unsigned int read = 0;
        result = sound->readData(sampleBuffer.data(), numChannels * sizeof(short), &read);
        if (result != FMOD_OK && result != FMOD_ERR_FILE_EOF)
        {
            ERRCHECK(result);
            break;
        }

        // Calculer la moyenne des canaux
        float sampleValue = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            sampleValue += static_cast<float>(sampleBuffer[ch]) / 32768.0f;
        }
        sampleValue /= numChannels;
        track.waveform[i] = sampleValue;
    }

    sound->release();
}

int main(int argc, char** argv)
{
    // SDL
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    if (!init_sdl(&window, &gl_context, "Theater Sound Manager", 1280, 720))
    {
        return -1;
    }

    // FMOD
    FMOD::System* fmodSystem = nullptr;
    FMOD_RESULT result;

    result = FMOD::System_Create(&fmodSystem);
    ERRCHECK(result);

    result = fmodSystem->init(512, FMOD_INIT_NORMAL, nullptr);
    ERRCHECK(result);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	//Io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	//Io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableSetMousePos;
	io.ConfigDockingWithShift = true;

    // Style personnalisé
    //ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 5.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 5.0f;

    // Palette de couleurs inspirée de GreenHorusTheme avec une pointe de couleur pastel
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.105f, 0.11f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.39f, 0.4f, 0.41f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(1.0f, 0.6f, 0.7f, 1.0f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 0.6f, 0.7f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.35f, 0.63f, 0.39f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.35f, 0.63f, 0.39f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.63f, 0.39f, 1.0f);

    // Couleur dominante pastel (par exemple, un bleu doux)
    ImVec4 dominantColor = ImVec4(0.5f, 0.7f, 0.9f, 1.0f);
    colors[ImGuiCol_SliderGrab] = dominantColor;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.4f, 0.6f, 0.8f, 1.0f);
    colors[ImGuiCol_CheckMark] = dominantColor;
    colors[ImGuiCol_TextSelectedBg] = dominantColor;
    colors[ImGuiCol_PlotLines] = dominantColor;
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.4f, 0.6f, 0.8f, 1.0f);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 5.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Variables pour la gestion des musiques
    std::vector<MusicTrack> musicTracks;
    int currentTrackIndex = -1;
    bool isPlaying = false;
    float volume = 1.0f; // Volume entre 0.0f et 1.0f

    // Variables pour la lecture des segments
    Uint32 segmentStartTime = 0;
    int segmentDurationSec = 120; // 2 minutes en secondes
    int fadeDurationSec = 5;      // 5 secondes
    Uint32 segmentDuration = segmentDurationSec * 1000; // en millisecondes
    Uint32 fadeDuration = fadeDurationSec * 1000;       // en millisecondes

    // Générateur aléatoire
    std::default_random_engine generator(std::chrono::system_clock::now().time_since_epoch().count());

    // Variables pour gérer le fondu enchaîné
    bool isFadingOut = false;
    bool isFadingIn = false;
    Uint32 fadeStartTime = 0;
    int nextTrackIndex = -1;

    FMOD::Channel* currentChannel = nullptr;
    FMOD::Channel* nextChannel = nullptr;

    // Options de lecture
    bool isRandomOrder = true;           // Lecture aléatoire des musiques
    bool isRandomSegment = false;        // Lecture de segments aléatoires
    bool dontPlayTwice = false;          // Ne pas rejouer les musiques déjà jouées
    bool useTimer = false;               // Utiliser un timer pour arrêter la lecture
    int timerDurationMin = 10;           // Durée du timer en minutes
    Uint32 timerStartTime = 0;
    std::vector<int> playedTracks;       // Liste des musiques déjà jouées

    // Main loop
    bool running = true;

    ImVec4 clear_color = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);

    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        // Gestion des segments et des transitions
        if (isPlaying && currentTrackIndex >= 0)
        {
            unsigned int channelPosition = 0;
            if (currentChannel)
            {
                result = currentChannel->getPosition(&channelPosition, FMOD_TIMEUNIT_MS);
                ERRCHECK(result);
            }

            Uint32 currentTime = SDL_GetTicks();
            Uint32 elapsedTime = currentTime - segmentStartTime;

            // Commencer la transition si nécessaire
            if (!isFadingOut && elapsedTime >= (segmentDuration - fadeDuration))
            {
                // Commencer le fondu sortant de la musique actuelle
                isFadingOut = true;
                fadeStartTime = SDL_GetTicks();

                // Sélectionner la prochaine musique
                if (!musicTracks.empty())
                {
                    if (dontPlayTwice)
                    {
                        if (playedTracks.size() == musicTracks.size())
                        {
                            playedTracks.clear();
                        }

                        std::vector<int> remainingTracks;
                        for (int i = 0; i < musicTracks.size(); ++i)
                        {
                            if (std::find(playedTracks.begin(), playedTracks.end(), i) == playedTracks.end())
                            {
                                remainingTracks.push_back(i);
                            }
                        }

                        if (!remainingTracks.empty())
                        {
                            if (isRandomOrder)
                            {
                                std::uniform_int_distribution<int> distribution(0, remainingTracks.size() - 1);
                                nextTrackIndex = remainingTracks[distribution(generator)];
                            }
                            else
                            {
                                nextTrackIndex = (currentTrackIndex + 1) % musicTracks.size();
                                if (std::find(remainingTracks.begin(), remainingTracks.end(), nextTrackIndex) == remainingTracks.end())
                                {
                                    nextTrackIndex = remainingTracks.front();
                                }
                            }
                            playedTracks.push_back(nextTrackIndex);
                        }
                        else
                        {
                            nextTrackIndex = currentTrackIndex;
                        }
                    }
                    else
                    {
                        if (isRandomOrder)
                        {
                            nextTrackIndex = currentTrackIndex;
                            while (nextTrackIndex == currentTrackIndex && musicTracks.size() > 1)
                            {
                                std::uniform_int_distribution<int> distribution(0, musicTracks.size() - 1);
                                nextTrackIndex = distribution(generator);
                            }
                        }
                        else
                        {
                            nextTrackIndex = (currentTrackIndex + 1) % musicTracks.size();
                        }
                    }

                    // Charger la prochaine musique
                    result = fmodSystem->playSound(musicTracks[nextTrackIndex].sound, nullptr, true, &nextChannel);
                    ERRCHECK(result);

                    result = nextChannel->setVolume(0.0f);
                    ERRCHECK(result);

                    // Si lecture de segments aléatoires
                    if (isRandomSegment)
                    {
                        unsigned int soundLength = 0;
                        musicTracks[nextTrackIndex].sound->getLength(&soundLength, FMOD_TIMEUNIT_MS);

                        if (segmentDuration < soundLength)
                        {
                            std::uniform_int_distribution<unsigned int> distribution(0, soundLength - segmentDuration);
                            unsigned int startPosition = distribution(generator);

                            result = nextChannel->setPosition(startPosition, FMOD_TIMEUNIT_MS);
                            ERRCHECK(result);
                        }
                        else
                        {
                            // Si le segment est plus long que la musique, commencer au début
                            result = nextChannel->setPosition(0, FMOD_TIMEUNIT_MS);
                            ERRCHECK(result);
                        }
                    }
                    else
                    {
                        // Commencer au début
                        result = nextChannel->setPosition(0, FMOD_TIMEUNIT_MS);
                        ERRCHECK(result);
                    }

                    result = nextChannel->setPaused(false);
                    ERRCHECK(result);

                    isFadingIn = true;
                }
            }

            // Gestion des fondues
            if (isFadingOut)
            {
                Uint32 fadeElapsed = SDL_GetTicks() - fadeStartTime;
                float fadeProgress = (float)fadeElapsed / (float)fadeDuration;

                if (fadeProgress >= 1.0f)
                {
                    fadeProgress = 1.0f;
                    isFadingOut = false;
                    isFadingIn = false;

                    // Arrêter la musique actuelle
                    if (currentChannel)
                    {
                        result = currentChannel->stop();
                        ERRCHECK(result);
                    }

                    // Passer à la musique suivante
                    currentChannel = nextChannel;
                    nextChannel = nullptr;
                    currentTrackIndex = nextTrackIndex;
                    segmentStartTime = SDL_GetTicks();
                }

                // Ajuster les volumes
                float currentVol = (1.0f - fadeProgress) * volume;
                float nextVol = fadeProgress * volume;

                if (currentChannel)
                {
                    result = currentChannel->setVolume(currentVol);
                    ERRCHECK(result);
                }

                if (nextChannel)
                {
                    result = nextChannel->setVolume(nextVol);
                    ERRCHECK(result);
                }
            }
            else if (isFadingIn)
            {
                Uint32 fadeElapsed = SDL_GetTicks() - fadeStartTime;
                float fadeProgress = (float)fadeElapsed / (float)fadeDuration;

                if (fadeProgress >= 1.0f)
                {
                    fadeProgress = 1.0f;
                    isFadingIn = false;
                }

                float currentVol = fadeProgress * volume;

                if (currentChannel)
                {
                    result = currentChannel->setVolume(currentVol);
                    ERRCHECK(result);
                }
            }
            else
            {
                // Maintenir le volume de la piste actuelle
                if (currentChannel)
                {
                    result = currentChannel->setVolume(volume);
                    ERRCHECK(result);
                }
            }
        }

        // Vérification du timer
        if (useTimer && timerStartTime > 0)
        {
            Uint32 elapsedTimer = SDL_GetTicks() - timerStartTime;
            if (elapsedTimer >= timerDurationMin * 60000)
            {
                // Arrêter la lecture
                if (currentChannel)
                {
                    result = currentChannel->stop();
                    ERRCHECK(result);
                    currentChannel = nullptr;
                    isPlaying = false;
                }
                timerStartTime = 0; // Réinitialiser le timer
            }
        }

        // Mettre à jour FMOD
        result = fmodSystem->update();
        ERRCHECK(result);

        // Démarrer une nouvelle frame ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Dockspace
        const ImGuiWindowFlags dockspace_flags = ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_MenuBar |
		ImGuiWindowFlags_AlwaysAutoResize;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (ImGui::Begin("MainDockSpace", nullptr, dockspace_flags))
        {
            //ImGui::PopStyleVar(3);
            ImGui::End();
        }

        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");   
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

        // Interface utilisateur
        ImGui::Begin("Theater Sound Manager");

        // Bouton pour ajouter des musiques via le sélecteur de fichiers natif
        if (ImGui::Button("Ajouter des musiques"))
{
    std::vector<std::string> selectedFiles = OpenFileDialog();
    for (const auto& filepath : selectedFiles)
    {
        std::string filename = fs::path(filepath).filename().string();

        // Créer le son en streaming pour économiser la mémoire
        FMOD::Sound* sound = nullptr;
        result = fmodSystem->createSound(filepath.c_str(), FMOD_DEFAULT | FMOD_CREATESTREAM, nullptr, &sound);
        if (result == FMOD_OK)
        {
            {
                std::lock_guard<std::mutex> lock(musicTracksMutex);
                musicTracks.push_back({ filepath, filename, sound });
            }

            // Générer la forme d'onde dans un thread séparé
            std::thread waveformThread(GenerateWaveform, fmodSystem, std::ref(musicTracks.back()), std::ref(musicTracksMutex));
            waveformThread.detach();
        }
        else
        {
            std::cerr << "Erreur FMOD::createSound: " << FMOD_ErrorString(result) << std::endl;
        }
    }
}


        // Affichage de la playlist avec options
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Playlist :");
        static int selectedTrackIndex = -1;

        // Utiliser un enfant pour rendre la playlist scrollable
        ImGui::BeginChild("PlaylistChild", ImVec2(0, 200), true, ImGuiWindowFlags_HorizontalScrollbar);

        if (ImGui::BeginTable("PlaylistTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Titre", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Sélection", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < musicTracks.size(); ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                const bool is_selected = (selectedTrackIndex == static_cast<int>(i));
                if (ImGui::Selectable(musicTracks[i].filename.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    selectedTrackIndex = static_cast<int>(i);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(static_cast<int>(i));

                if (ImGui::Button("Suppr"))
                {
                    std::lock_guard<std::mutex> lock(musicTracksMutex);
                    musicTracks[i].sound->release();
                    musicTracks.erase(musicTracks.begin() + i);
                    if (selectedTrackIndex == static_cast<int>(i))
                        selectedTrackIndex = -1;
                    if (currentTrackIndex == static_cast<int>(i))
                    {
                        if (currentChannel)
                        {
                            result = currentChannel->stop();
                            ERRCHECK(result);
                            currentChannel = nullptr;
                        }
                        isPlaying = false;
                    }
                    --i; // Ajuster l'index après suppression
                }
                ImGui::SameLine();
                if (ImGui::Button("Haut") && i > 0)
                {
                    std::swap(musicTracks[i], musicTracks[i - 1]);
                    if (selectedTrackIndex == static_cast<int>(i))
                        selectedTrackIndex = i - 1;
                    else if (selectedTrackIndex == static_cast<int>(i - 1))
                        selectedTrackIndex = i;
                }
                ImGui::SameLine();
                if (ImGui::Button("Bas") && i < musicTracks.size() - 1)
                {
                    std::swap(musicTracks[i], musicTracks[i + 1]);
                    if (selectedTrackIndex == static_cast<int>(i))
                        selectedTrackIndex = i + 1;
                    else if (selectedTrackIndex == static_cast<int>(i + 1))
                        selectedTrackIndex = i;
                }

                ImGui::PopID();

                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button("Sélect."))
                {
                    selectedTrackIndex = static_cast<int>(i);
                }
            }
            ImGui::EndTable();
        }

        ImGui::EndChild();

       // ImGui::PopStyleVar(2);

        // Contrôles de lecture
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Contrôles de lecture :");

        if (ImGui::Button("Précédent"))
        {
            if (!musicTracks.empty())
            {
                // Arrêter la musique actuelle
                if (currentChannel)
                {
                    result = currentChannel->stop();
                    ERRCHECK(result);
                }

                // Passer à la musique précédente
                if (isRandomOrder)
                {
                    std::uniform_int_distribution<int> distribution(0, musicTracks.size() - 1);
                    currentTrackIndex = distribution(generator);
                }
                else
                {
                    currentTrackIndex = (currentTrackIndex - 1 + musicTracks.size()) % musicTracks.size();
                }

                // Jouer la nouvelle musique
                result = fmodSystem->playSound(musicTracks[currentTrackIndex].sound, nullptr, false, &currentChannel);
                ERRCHECK(result);
                result = currentChannel->setVolume(volume);
                ERRCHECK(result);
                isPlaying = true;
                segmentStartTime = SDL_GetTicks();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Jouer"))
        {
            if (!isPlaying && selectedTrackIndex >= 0)
            {
                currentTrackIndex = selectedTrackIndex;
                result = fmodSystem->playSound(musicTracks[currentTrackIndex].sound, nullptr, true, &currentChannel);
                ERRCHECK(result);

                // Commencer avec un volume de 0 pour le fondu
                result = currentChannel->setVolume(0.0f);
                ERRCHECK(result);

                result = currentChannel->setPaused(false);
                ERRCHECK(result);

                isPlaying = true;
                segmentStartTime = SDL_GetTicks();

                // Démarrer le fondu d'entrée
                isFadingIn = true;
                fadeStartTime = SDL_GetTicks();

                if (useTimer)
                {
                    timerStartTime = SDL_GetTicks();
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Pause"))
        {
            if (currentChannel)
            {
                bool paused;
                result = currentChannel->getPaused(&paused);
                ERRCHECK(result);
                result = currentChannel->setPaused(!paused);
                ERRCHECK(result);
                isPlaying = !paused;
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            if (currentChannel)
            {
                result = currentChannel->stop();
                ERRCHECK(result);
                currentChannel = nullptr;
                isPlaying = false;
                timerStartTime = 0;
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Suivant"))
        {
            if (!musicTracks.empty())
            {
                // Arrêter la musique actuelle
                if (currentChannel)
                {
                    result = currentChannel->stop();
                    ERRCHECK(result);
                }

                // Passer à la musique suivante
                if (isRandomOrder)
                {
                    std::uniform_int_distribution<int> distribution(0, musicTracks.size() - 1);
                    currentTrackIndex = distribution(generator);
                }
                else
                {
                    currentTrackIndex = (currentTrackIndex + 1) % musicTracks.size();
                }

                // Jouer la nouvelle musique
                result = fmodSystem->playSound(musicTracks[currentTrackIndex].sound, nullptr, false, &currentChannel);
                ERRCHECK(result);
                result = currentChannel->setVolume(volume);
                ERRCHECK(result);
                isPlaying = true;
                segmentStartTime = SDL_GetTicks();
            }
        }

        // Contrôle du volume
        ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f);

        // Paramètres personnalisables
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Paramètres :");
        ImGui::SliderInt("Durée du segment (secondes)", &segmentDurationSec, 10, 600);
        ImGui::SliderInt("Durée du fondu (secondes)", &fadeDurationSec, 1, 30);
        // Mettre à jour les durées en millisecondes
        segmentDuration = segmentDurationSec * 1000;
        fadeDuration = fadeDurationSec * 1000;

        // Options de lecture
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Options de lecture :");
        ImGui::Checkbox("Lecture aléatoire des musiques", &isRandomOrder);
        ImGui::Checkbox("Lecture de segments aléatoires", &isRandomSegment);
        ImGui::Checkbox("Don't play twice", &dontPlayTwice);
        ImGui::Checkbox("Utiliser le timer", &useTimer);
        if (useTimer) {
            ImGui::SliderInt("Durée du timer (minutes)", &timerDurationMin, 1, 120);
        }

        // Barre de progression et informations sur la lecture
        ImGui::Spacing();
        ImGui::Separator();
        if (isPlaying && currentTrackIndex >= 0)
        {
            unsigned int channelPosition = 0;
            if (currentChannel)
            {
                result = currentChannel->getPosition(&channelPosition, FMOD_TIMEUNIT_MS);
                ERRCHECK(result);
            }

            Uint32 currentTime = SDL_GetTicks();
            int elapsedTime = currentTime - segmentStartTime;
            int remainingTime = (segmentDuration - elapsedTime) / 1000;
            if (remainingTime < 0) remainingTime = 0;

            int totalSeconds = remainingTime;
            int minutes = totalSeconds / 60;
            int seconds = totalSeconds % 60;

            ImGui::Text("Lecture en cours : %s", musicTracks[currentTrackIndex].filename.c_str());

            // Barre de progression
            float progress = static_cast<float>(elapsedTime) / static_cast<float>(segmentDuration);
            if (progress > 1.0f) progress = 1.0f;

            if (!musicTracks[currentTrackIndex].waveform.empty())
            {
                ImGui::PlotLines("##Waveform", musicTracks[currentTrackIndex].waveform.data(),
                    musicTracks[currentTrackIndex].waveform.size(), 0, nullptr, -1.0f, 1.0f, ImVec2(0, 80));

                // Dessiner le curseur de position
                unsigned int soundLength = 0;
                musicTracks[currentTrackIndex].sound->getLength(&soundLength, FMOD_TIMEUNIT_MS);

                float waveformProgress = static_cast<float>(channelPosition) / static_cast<float>(soundLength);
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float waveformWidth = ImGui::GetContentRegionAvail().x;
                float cursorX = pos.x + waveformWidth * waveformProgress;
                draw_list->AddLine(ImVec2(cursorX, pos.y), ImVec2(cursorX, pos.y + 80), IM_COL32(255, 0, 0, 255));
            }
            else
            {
                ImGui::ProgressBar(progress, ImVec2(0.0f, 0.0f));
            }

            ImGui::Text("Temps restant sur le segment : %d min %02d s", minutes, seconds);
        }
        else
        {
            ImGui::Text("Aucune musique en cours de lecture.");
        }

        ImGui::End();

        // Rendu ImGui
        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
        }

        SDL_GL_SwapWindow(window);
    }

    // Nettoyage
    for (auto& track : musicTracks)
    {
        if (track.sound)
        {
            track.sound->release();
        }
    }

    fmodSystem->close();
    fmodSystem->release();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    cleanup_sdl(window, gl_context);
    return 0;
}
