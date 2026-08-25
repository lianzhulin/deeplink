/* demo host: just link the firmware objects for a realistic ELF */
int firmware_init(void);
int firmware_verify(const unsigned char *data, int len);
int main(void) {
    return firmware_init();
}
