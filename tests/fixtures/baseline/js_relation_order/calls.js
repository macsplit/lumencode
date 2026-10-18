function createApp(options) {
  return { options };
}

describe('request suite', function () {
  before(function () {
    this.app = createApp();
  });

  describe('with inflate option', function () {
    describe('when false', function () {
      before(function () {
        this.app = createApp({ inflate: false });
      });
      it('accepts a configured app', function () {
        check(this.app);
      });
    });
  });
});
