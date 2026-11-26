-- pgbench script for the collection-overhead experiment: one small insert per transaction.
INSERT INTO lab_app.orders (amount) VALUES (random() * 100);
