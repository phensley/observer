
import java.util.concurrent.atomic.AtomicBoolean;

public class Test {

    static final AtomicBoolean flag = new AtomicBoolean(false);

    static class Busy implements Runnable {

        private int value;

        private void runFoo() {
            runBar();
        }

        private void runBar() {
            value++;
            value *= 0.75;
            runBaz();
        }

        private void runBaz() {
            runQuux();
        }

        private void runQuux() {
            sleep();
        }

        private void sleep() {
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) { }
        }

        @Override
        public void run() {
            while (!flag.get()) {
                runFoo();
            }
        }

    }

    static class Idle implements Runnable {

        public void run() {
            while (!flag.get()) {
                try {
                    Thread.sleep(500);
                } catch (InterruptedException e) { }
            }
        }
    }

    public static void main(String[] args) throws Exception {
        int limit = 10;
        if (args.length > 0) {
            limit = Integer.parseInt(args[0]);
        }
        Thread[] threads = new Thread[limit];
        for (int i = 0; i < limit; i++) {
            try {
                Runnable runnable = (i % 100 == 0) ? new Busy() : new Idle();
                threads[i] = new Thread(runnable, "client-" + (i + 1));
                threads[i].start();

            } catch (OutOfMemoryError error) {
                System.err.println("FATAL! " + error);
                flag.set(true);
                System.exit(1);
            }
        }
        Thread.sleep(3500);
        flag.set(true);
    }

}

