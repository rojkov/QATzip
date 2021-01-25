#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <strings.h>
#include <qatzip.h>

#define CHUNK_SIZE 4096

QzSession_T qz_sess;
QzSessionParams_T qz_params = {(QzHuffmanHdr_T)0,};

struct Context {
	int avail_in;
	int avail_out;
	QzStream_T stream;
	QzSession_T* sess;
	unsigned char* chunk;
};

bool process(struct Context *ctx, FILE* out, int last) {
	ctx->stream.in_sz = ctx->avail_in;
	ctx->stream.out_sz = ctx->avail_out;
	printf(" *** Before qzDecompressStream(last=%d) stream.in_sz: %d, stream.out_sz: %d\n", last, ctx->stream.in_sz, ctx->stream.out_sz);
	int qz_status = qzDecompressStream(ctx->sess, &(ctx->stream), last);
	printf(" *** After qzDecompressStream(last=%d) stream.in_sz: %d, stream.out_sz: %d\n", last, ctx->stream.in_sz, ctx->stream.out_sz);
	if (qz_status != QZ_OK) {
		printf("Ooops status: %d\n", qz_status);
		return false;
	}

	ctx->avail_out -= ctx->stream.out_sz;
	ctx->avail_in -= ctx->stream.in_sz;
	ctx->stream.out = ctx->stream.out + ctx->stream.out_sz;
	ctx->stream.in = ctx->stream.in + ctx->stream.in_sz;

	if (ctx->avail_out == 0) {
		int written = fwrite(ctx->chunk, 1, CHUNK_SIZE, out);
		if (written != CHUNK_SIZE) {
			printf("E: failed to write\n");
			return false;
		}
		ctx->avail_out = CHUNK_SIZE;
		ctx->stream.out = ctx->chunk;
	}

	return true;
}

int main(int argc, char **argv) {
	int rc = 0;
	int qz_status;
	char filename[] = "blob.json.gz";

	if (qzGetDefaults(&qz_params) != QZ_OK) {
		printf("E: unable to set default params\n");
		return -1;
	}

	qz_params.direction = QZ_DIR_DECOMPRESS;
	qz_params.hw_buff_sz = 1024 * 64;
	qz_params.sw_backup = 0;

	qz_status = qzInit(&qz_sess, qz_params.sw_backup);
	if (qz_status != QZ_OK && qz_status != QZ_DUPLICATE) {
		printf("E: unable to init hardware\n");
		return -1;
	}

	qz_status = qzSetupSession(&qz_sess, &qz_params);
	if (qz_status != QZ_OK && qz_status != QZ_DUPLICATE) {
		printf("E: unable to setup Qatzip session \n");
		return -1;
	}

	if (filename == NULL) {
		printf("E: file not set\n");
		rc = -1;
		goto exit;
	}

	struct stat st;
	rc = stat(filename, &st);
	if (rc != 0) {
		printf("E: can't check file %s\n", filename);
		goto exit;
	}
	unsigned int size = st.st_size;
	printf("File size of %s is %d\n", filename, size);

	unsigned char* blob = malloc(size);
	if (blob == NULL) {
		printf("E: failed to allocate mem for input\n");
		rc = -1;
		goto exit;
	}
	unsigned char* chunk = malloc(CHUNK_SIZE);
	if (chunk == NULL) {
		printf("E: failed to allocate mem for output buffer\n");
		rc = -1;
		goto exit;
	}
	bzero(chunk, CHUNK_SIZE);

	FILE *f = fopen(filename, "rb");
	unsigned int count = fread(blob, 1, size, f);
	fclose(f);
	if (count != size) {
		printf("E: failed to read full file - %d out of %d\n", count, size);
		rc = -1;
		goto done;
	}


	FILE *f_out = fopen("/tmp/out123", "wb");

	int slice_sizes[] = {20416, 64, 20416, 12224, 8128, 16320, 7865, 0};
	struct Context ctx;
	ctx.stream.in = blob;
	ctx.stream.out = chunk;
	ctx.chunk = chunk;
	ctx.avail_out = CHUNK_SIZE;
	ctx.sess = &qz_sess;
	int acc = 0;

	for (int i = 0; slice_sizes[i] != 0; i++) {
		ctx.avail_in = slice_sizes[i];
		ctx.stream.in = blob + acc;
		printf("New slice of size %d\n", ctx.avail_in);
		while (ctx.avail_in > 0) {
			if (!process(&ctx, f_out, 0)) {
				goto done;
			}
		}
		acc += slice_sizes[i];
	}

	bool success;
	do {
		success = process(&ctx, f_out, 1);
	} while (success && ctx.stream.pending_out > 0);

	const int n_output = CHUNK_SIZE - ctx.avail_out;
	if (n_output > 0) {
		int written = fwrite(chunk, 1, n_output, f_out);
		if (written != n_output) {
			printf("E: unable to write the rest\n");
		}
	}

	fclose(f_out);

done:
	free(chunk);
	free(blob);

exit:
	qzTeardownSession(&qz_sess);
	qzClose(&qz_sess);
	return rc;
}
