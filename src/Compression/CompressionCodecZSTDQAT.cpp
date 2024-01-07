#ifdef ENABLE_ZSTD_QAT_CODEC
#include <Common/logger_useful.h>
#include <Compression/CompressionCodecZSTD.h>
#include <Compression/CompressionFactory.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/IAST.h>

#include <qatseqprod.h>
#include <zstd.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
}

/// Hardware-accelerated ZSTD. Supports only compression so far.
class CompressionCodecZSTDQAT : public CompressionCodecZSTD
{
public:
    static constexpr auto ZSTDQAT_SUPPORTED_MIN_LEVEL = 1;
    static constexpr auto ZSTDQAT_SUPPORTED_MAX_LEVEL = 12;

    explicit CompressionCodecZSTDQAT(int level_);
    ~CompressionCodecZSTDQAT() override;

protected:
    bool isZstdQat() const override { return true; }
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

private:
    const int level;
    ZSTD_CCtx * cctx;
    void * sequenceProducerState;
    Poco::Logger * log;
};

UInt32 CompressionCodecZSTDQAT::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    size_t compressed_size = ZSTD_compress2(cctx, dest, ZSTD_compressBound(source_size), source, source_size);

    if (ZSTD_isError(compressed_size))
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with ZSTD_QAT codec: {}", ZSTD_getErrorName(compressed_size));

    return static_cast<UInt32>(compressed_size);
}

void registerCodecZSTDQAT(CompressionCodecFactory & factory)
{
    factory.registerCompressionCodec("ZSTD_QAT", {}, [&](const ASTPtr & arguments) -> CompressionCodecPtr
    {
        int level = CompressionCodecZSTD::ZSTD_DEFAULT_LEVEL;
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 1)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "ZSTD_QAT codec must have 1 parameter, given {}", arguments->children.size());

            const auto children = arguments->children;
            const auto * literal = children[0]->as<ASTLiteral>();
            if (!literal)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "ZSTD_QAT codec argument must be integer");

            level = static_cast<int>(literal->value.safeGet<UInt64>());
            if (level < CompressionCodecZSTDQAT::ZSTDQAT_SUPPORTED_MIN_LEVEL || level > CompressionCodecZSTDQAT::ZSTDQAT_SUPPORTED_MAX_LEVEL )
                /// that's a hardware limitation
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
                    "ZSTDQAT codec doesn't support level more than {} and lower than {} , given {}",
                    CompressionCodecZSTDQAT::ZSTDQAT_SUPPORTED_MAX_LEVEL, CompressionCodecZSTDQAT::ZSTDQAT_SUPPORTED_MIN_LEVEL, level);
        }

        return std::make_shared<CompressionCodecZSTDQAT>(level);
    });
}

CompressionCodecZSTDQAT::CompressionCodecZSTDQAT(int level_)
    : CompressionCodecZSTD(level_)
    , level(level_)
    , log(&Poco::Logger::get("CompressionCodecZSTDQAT"))
{
    setCodecDescription("ZSTD_QAT", {std::make_shared<ASTLiteral>(static_cast<UInt64>(level))});

    cctx = ZSTD_createCCtx();

    int res = QZSTD_startQatDevice();
    LOG_DEBUG(log, "Initialization of ZSTD_QAT codec, status: {} ", res);

    sequenceProducerState = QZSTD_createSeqProdState();

    ZSTD_registerSequenceProducer(
        cctx,
        sequenceProducerState,
        qatSequenceProducer
    );

    ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableSeqProducerFallback, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
}

CompressionCodecZSTDQAT::~CompressionCodecZSTDQAT()
{
    if (sequenceProducerState != nullptr)
    {
        QZSTD_freeSeqProdState(sequenceProducerState);
        sequenceProducerState = nullptr;
    }

    if (cctx != nullptr)
    {
        size_t status = ZSTD_freeCCtx(cctx);
        if (status != 0)
            LOG_WARNING(log, "ZSTD_freeCCtx failed with status: {} ", status);
        cctx = nullptr;
    }
}

}
#endif
