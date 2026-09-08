#include "pimbalgame/Music.hpp"

namespace pimbalgame
{
Music::Music() = default;
Music::~Music() = default;

bool Music::load(const std::filesystem::path& audioPath)
{
    // SFML's Audio module is built against the system Vorbis/FLAC/Ogg libraries,
    // so .ogg files decode natively here and no bespoke codec is needed.
    if (!mBuffer.loadFromFile(audioPath))
    {
        return false;
    }

    mSound = std::make_unique<sf::Sound>(mBuffer);
    mSound->setLooping(true);

    mValid = true;
    return true;
}

void Music::play()
{
    if (mValid && mSound)
    {
        mSound->play();
    }
}

void Music::pause()
{
    if (mSound)
    {
        mSound->pause();
    }
}

void Music::stop()
{
    if (mSound)
    {
        mSound->stop();
    }
}

void Music::setVolume(float volume)
{
    if (mSound)
    {
        mSound->setVolume(volume);
    }
}

bool Music::isPlaying() const
{
    return mValid && mSound && mSound->getStatus() == sf::SoundSource::Status::Playing;
}
} // namespace pimbalgame
